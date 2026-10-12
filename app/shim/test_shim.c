// Self-check for the shim: NAL splitter, both codecs, fan-out ring, settings file, control channel.
#include "libkvm_shim.c"
#include <assert.h>
#include <fcntl.h>

// Pipe a file through a socket pair so feed() sees a real socket (it writes control lines back on it).
static void *writer(void *arg) {
  int *a = arg; char b[65536]; ssize_t n;
  while ((n = read(a[0], b, sizeof b)) > 0) { if (write(a[1], b, n) != n) break; }
  close(a[0]); shutdown(a[1], SHUT_WR); return NULL;   // half-close: feed() sees EOF but can still send control lines
}
static void feed_file(const char *path) {
  int sv[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  static int a[2]; a[0] = open(path, O_RDONLY); a[1] = sv[1]; assert(a[0] >= 0);
  pthread_t th; pthread_create(&th, NULL, writer, a);
  feed(sv[0], -1); close(sv[0]); pthread_join(th, NULL); close(sv[1]);
}

// Drain everything visible to the calling thread; returns frames read, checks keyframe samples.
static int consume(int type, int off, int *first) {
  int total = 0, r; uint8_t *p; uint32_t n; *first = -1;
  while ((r = kvmv_read_img(0, 0, type, 0, &p, &n)) >= 0) {
    assert((r == 3 + off || r == 4 + off) && n > 5 && !memcmp(p, "\0\0\0\1", 4));
    if (*first < 0) *first = r;
    if (r == 3 + off) {  // keyframe sample is self-contained: parameter sets then the IDR slice
      if (off) { assert(p[4] == 0x40); assert(memmem(p, n, "\0\0\0\1\x42", 5) && memmem(p, n, "\0\0\0\1\x44", 5)); }
      else     { assert((p[4] & 0x1f) == 7); int sawpps = 0, sawidr = 0;                 // SPS first, then PPS and IDR by type
                 for (uint32_t i = 4; i + 5 <= n; i++) if (!memcmp(p + i, "\0\0\0\1", 4)) { int ty = p[i+4] & 0x1f; sawpps |= ty == 8; sawidr |= ty == 5; }
                 assert(sawpps && sawidr); }
    }
    total++;
  }
  return total;
}
// the other stream loop asks with the other type and must see the same frames (nothing stolen)
static void *second_consumer(void *arg) { int first, off = ((int *)arg)[1]; *(int *)arg = consume(5, off, &first);  /* main consumer used type 1 */ assert(first == 3 + off); return NULL; }

static void run(const char *path, int off) {
  pthread_mutex_lock(&mu); drain_locked(); memset(cons, 0, sizeof cons); net_connected = 1; pthread_mutex_unlock(&mu);  // fresh consumers
  feed_file(path);  // 60 frames, GOP 30: a fresh consumer starts at the latest keyframe -> last 30 frames
  int first, got = consume(1, off, &first);
  printf("%s: frames=%d first=%d\n", path, got, first);
  assert(first == 3 + off && got == 30);
  // another thread is a separate consumer and sees the same frames (fan-out, nothing stolen)
  int arg[2] = { off, off }; pthread_t th;  // runs consume() with the other type
  pthread_create(&th, NULL, second_consumer, arg); pthread_join(th, NULL);
  assert(arg[0] == 30);
}

static char *recv_all(int fd) {
  static char buf[1024]; ssize_t n = recv(fd, buf, sizeof buf - 1, MSG_DONTWAIT); buf[n > 0 ? n : 0] = 0; return buf;
}

int main(int argc, char **argv) {
  (void)argc;
  run(argv[1], 0);   // x264 stream, SPS header 0x67
  run(argv[2], 4);   // HEVC
  run(argv[3], 0);   // VideoToolbox stream, SPS header 0x27, right after HEVC: codec detection must flip back
  // the requested type does not filter (stock behaviour): H.264 asked while HEVC flows -> HEVC codes
  pthread_mutex_lock(&mu); drain_locked(); pthread_mutex_unlock(&mu);
  feed_file(argv[2]); uint8_t *p; uint32_t n;
  assert(kvmv_read_img(0, 0, 1, 0, &p, &n) == 7);
  // no client attached and no stock library: reads fail cleanly instead of crashing
  net_connected = 0;
  assert(kvmv_read_img(0, 0, 1, 0, &p, &n) == -1);

  // settings file -> control state
  const char *toml = "/tmp/kvm_shim_test_vin.toml";
  FILE *f = fopen(toml, "w"); assert(f);
  fputs("[device]\nenable = true\nmode = 'h265'\n\n[encoder]\nbitrate = 4000\nfps = 0\ngop = 50\n", f); fclose(f);
  setenv("KVM_SHIM_VIN_TOML", toml, 1);
  usleep(300000);  // the feed loop polled during run(); clear the 250 ms rate limit
  poll_vin_toml();
  assert(ctl.codec_hevc == 1 && ctl.fps == 30 && ctl.bitrate == 4000 && ctl.gop == 50);  // fps 0 -> 30

  // control channel: full state on connect, then only deltas, in order; pause/resume follow the last viewer read
  int sv[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  last_read = now_s();                                                 // a viewer read a frame just now -> "resume"
  assert(send_control(sv[0], 1) == 0);
  char *got = recv_all(sv[1]);
  assert(!strcmp(got, "ver 1\ncodec h265\nfps 30\nbitrate 4000\ngop 50\nresume\n"));
  assert(send_control(sv[0], 0) == 0 && recv_all(sv[1])[0] == 0);   // nothing changed, nothing sent
  kvmv_force_idr(); ctl.codec_hevc = 0; last_read = now_s() - PAUSE_AFTER_S - 1;   // nobody read for too long -> "pause"
  assert(send_control(sv[0], 0) == 0 && !strcmp(recv_all(sv[1]), "codec h264\npause\nidr\n"));
  net_connected = 1; kvmv_read_img(0, 0, 1, 0, &p, &n);              // a viewer polls again
  assert(send_control(sv[0], 0) == 0 && !strcmp(recv_all(sv[1]), "resume\n"));
  close(sv[1]); assert(send_control(sv[0], 1) == -1);                 // peer gone: reported, no SIGPIPE
  unlink(toml);
  puts("OK");
}

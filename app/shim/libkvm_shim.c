// Proxy libkvm.so for NanoKVM-Server (NanoKVM Go+).
//
// Loads the stock library from the same directory and forwards every call to it, so the KVM keeps
// working as shipped. While a client is connected on TCP :5555 (the Mac over the USB NIC), video
// frames come from that stream instead: raw Annex-B H.264 or H.265, split into NALs and handed to
// the server with the stock library's return codes (1-4 H.264, 5-8 H.265).
//
// Codec and encoder settings come from /etc/kvm/kvm_vin.toml, which the server rewrites on every
// change and the stock video daemon reads too; the shim polls it and relays the values. Like the
// stock library, read_img serves every caller whatever is flowing (the server runs more than one
// stream loop during a mode switch); each calling thread has its own cursor over a shared ring.
//
// Control channel, KVM -> client, text lines on the same socket:
//   ver 1 | codec h264|h265 | fps N | bitrate N(kbit/s) | gop N | idr | pause | resume
// The client sends only the video byte stream.
//
// Build: gcc -O2 -shared -fPIC -Wl,-soname,libkvm.so.0 -o libkvm.so libkvm_shim.c -ldl -lpthread
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define IMG_NOT_EXIST -1
#define QCAP 256          // ponytail: fixed ring, drop-oldest when full
#define RDBUF (1 << 20)
#define PIC_MAX (4 << 20)  // one picture's slices; a runaway picture is cut here
#define PAUSE_AFTER_S 15  // no frame reads for this long = no viewers -> "pause"; the first read after that -> "resume".
                          // Also the connect dump's rule. A server loop stops reading while a slow viewer blocks its
                          // websocket write, so keep this well above such stalls.
#define VIN_TOML "/etc/kvm/kvm_vin.toml"

// ---------------------------------------------------------------- stock library
static struct {
  void (*init)(uint8_t);
  void (*deinit)(void);
  int (*read_img)(uint16_t, uint16_t, uint8_t, uint16_t, uint8_t **, uint32_t *);
  int (*read_audio)(uint8_t **, uint32_t *);
  int (*set_fps)(uint8_t);
  int (*get_fps)(void);
  int (*set_gop)(uint16_t);
  int (*set_bitrate)(uint16_t);
  int (*set_rate_control)(uint8_t);
  int (*hdmi_control)(uint8_t);
  int (*force_idr)(void);
  void (*close_video_stream)(void);
  int (*reset_audio_capture)(void);
  int (*audio_playback_open)(uint32_t);
  int (*audio_playback_close)(void);
  int (*audio_playback_write_opus)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
} real;
static void *real_handle;

static void load_real(void) {
  const char *path = "libkvm.so.0.1.0";  // sibling of this .so
  char buf[512];
  Dl_info di;
  const char *slash;
  if (dladdr((void *)load_real, &di) && di.dli_fname && (slash = strrchr(di.dli_fname, '/'))) {
    snprintf(buf, sizeof buf, "%.*s/libkvm.so.0.1.0", (int)(slash - di.dli_fname), di.dli_fname);
    path = buf;
  }
  real_handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!real_handle) { fprintf(stderr, "libkvm_shim: stock library not loaded (%s): %s\n", path, dlerror()); return; }
#define LOAD(name) real.name = dlsym(real_handle, "kvmv_" #name)
  LOAD(init); LOAD(deinit); LOAD(read_img); LOAD(read_audio); LOAD(set_fps); LOAD(get_fps); LOAD(set_gop);
  LOAD(set_bitrate); LOAD(set_rate_control); LOAD(hdmi_control); LOAD(force_idr); LOAD(close_video_stream);
  LOAD(reset_audio_capture); LOAD(audio_playback_open); LOAD(audio_playback_close); LOAD(audio_playback_write_opus);
#undef LOAD
  fprintf(stderr, "libkvm_shim: stock library loaded from %s\n", path);
}

// ---------------------------------------------------------------- shared state
typedef struct { uint8_t *p; uint32_t n; int code; } nal_t;  // code = libkvm return value
static nal_t ring[QCAP];      // entries live until overwritten QCAP pushes later; readers hold no copies
static uint64_t seq_next;     // sequence number of the next push; ring holds [seq_next-QCAP, seq_next)
static nal_t vps, sps, pps;   // cached parameter sets
static nal_t pic;             // the picture being assembled: one sample per picture, whatever the encoder's slicing
static int hevc;              // codec of the incoming stream, detected from its parameter sets
static int net_connected;     // a client stream is attached: serve from the ring
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;

// One cursor per requested codec type (1 = H.264 loop, 5 = H.265 loop). The server's stream loops are
// goroutines, so consecutive calls from one loop arrive on different OS threads; the requested type is
// the only stable identity. A cursor idle for more than a second is treated as a new consumer.
static struct { uint64_t next; double last; } cons[2];

// Control state (from kvm_vin.toml, plus idr/pause); the feed loop sends whatever changed.
static struct { int codec_hevc, fps, bitrate, gop, idr; } ctl = { 0, 30, 8000, 30, 0 };
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static double last_read;  // seconds, now_s() clock: last viewer frame read. "paused" is derived from it, never stored.

// ---------------------------------------------------------------- NAL ring
enum { C_P = 0, C_KEY, C_SPS, C_PPS, C_VPS };
static const int base_code[] = { 4, 3, 1, 2 };  // C_P, C_KEY, C_SPS, C_PPS -> H.264 codes; +4 for H.265

static void push_locked(nal_t x) {
  nal_t *slot = &ring[seq_next % QCAP];
  free(slot->p);
  *slot = x;
  seq_next++;
  pthread_cond_broadcast(&cv);
}

static void drain_locked(void) {
  for (int i = 0; i < QCAP; i++) { free(ring[i].p); ring[i].p = NULL; }
  free(pic.p); pic = (nal_t){0};
  for (int i = 0; i < 2; i++) cons[i].next = seq_next;
}

static int is_key(int code) { return code == 3 || code == 7; }

// Sequence of the most recent keyframe in the ring, or seq_next if none.
static uint64_t latest_key_locked(void) {
  uint64_t lo = seq_next > QCAP ? seq_next - QCAP : 0;
  for (uint64_t s = seq_next; s > lo; s--) if (ring[(s - 1) % QCAP].p && is_key(ring[(s - 1) % QCAP].code)) return s - 1;
  return seq_next;
}

// Cursor for the requested type; a new (or long idle) consumer starts at the latest keyframe.
static int cursor_locked(uint8_t type) {
  int i = type == 5;
  double t = now_s();
  if (t - cons[i].last > 1.0) cons[i].next = latest_key_locked();
  cons[i].last = t;
  return i;
}

static int nal_type(uint8_t h) { return hevc ? (h >> 1) & 0x3f : h & 0x1f; }

// Classify a NAL by its header byte; -1 = drop (AUD, SEI, other non-VCL).
static int nal_class(const uint8_t *d) {
  int t = nal_type(d[0]);
  if (hevc) {
    switch (t) { case 32: return C_VPS; case 33: return C_SPS; case 34: return C_PPS; }
    if (t >= 16 && t <= 21) return C_KEY;   // BLA/IDR/CRA
    return t < 32 ? C_P : -1;
  }
  switch (t) { case 7: return C_SPS; case 8: return C_PPS; case 5: return C_KEY; }
  return (t >= 1 && t <= 4) ? C_P : -1;
}

// Appends [start code]NAL to x with one reallocation.
static void append(nal_t *x, const uint8_t *d, uint32_t n, int start_code) {
  x->p = realloc(x->p, x->n + n + (start_code ? 4 : 0));
  if (start_code) { memcpy(x->p + x->n, "\0\0\0\1", 4); x->n += 4; }
  memcpy(x->p + x->n, d, n);
  x->n += n;
}

// The assembled picture becomes one sample. Pushing is the only step that takes the lock: pic and the
// parameter-set caches belong to the feed thread (connect and disconnect reset them on that same thread).
static void flush_pic(void) {
  if (!pic.p) return;
  pthread_mutex_lock(&mu);
  push_locked(pic);
  pthread_mutex_unlock(&mu);
  pic = (nal_t){0};
}

// First slice of a picture: HEVC first_slice_segment_in_pic_flag, H.264 first_mb_in_slice == 0 (ue(v) 0 is a lone 1 bit).
static int first_slice(const uint8_t *d, uint32_t n) { return hevc ? n > 2 && (d[2] & 0x80) : n > 1 && (d[1] & 0x80); }
static int is_aud(uint8_t h) { return nal_type(h) == (hevc ? 35 : 9); }

// d/n = one NAL without start code.
// One sample per picture: encoders that slice (Qualcomm's HEVC makes 4 per picture) would otherwise hand the
// browser one fragment per frame message, which decodes to black. Slices collect in pic until the picture ends:
// the client's access-unit delimiter (caught early in feed()), any other non-slice NAL, or the next picture's
// first slice. Parameter sets are cached, never emitted on their own: the browser's direct-mode decoder only
// starts on a keyframe message and drops everything before it, so each keyframe sample is self-contained:
// [VPS][SPS][PPS][IDR slices]. WebRTC's packetizer splits the sample on start codes.
static void on_nal(const uint8_t *d, uint32_t n) {
  if (n == 0) return;
  // Codec detection from parameter sets. HEVC headers are two bytes with the second always 0x01 for
  // single-layer streams; an H.264 SPS has type 7 and its second byte is the profile (never 0x01).
  if (n >= 2 && d[0] == 0x40 && d[1] == 0x01) hevc = 1;              // HEVC VPS
  else if (n >= 2 && (d[0] & 0x9f) == 0x07 && d[1] != 0x01) hevc = 0; // H.264 SPS: 0x27 (VideoToolbox), 0x47, 0x67 (x264)
  int c = nal_class(d);
  if ((c != C_P && c != C_KEY) || first_slice(d, n)) flush_pic();
  if (c == C_VPS || c == C_SPS || c == C_PPS) {
    nal_t *cache = c == C_VPS ? &vps : c == C_SPS ? &sps : &pps;
    free(cache->p);
    *cache = (nal_t){0};
    append(cache, d, n, 1);
  } else if (c >= 0) {
    int code = base_code[c] + (hevc ? 4 : 0);
    if (!pic.p) {
      pic.code = code;
      if (c == C_KEY && sps.p && pps.p) {
        if (hevc && vps.p) append(&pic, vps.p, vps.n, 0);
        append(&pic, sps.p, sps.n, 0);
        append(&pic, pps.p, pps.n, 0);
      }
    } else if (c == C_KEY) pic.code = code;
    append(&pic, d, n, 1);
    if (pic.n > PIC_MAX) flush_pic();
  }
}

// Emit the retained tail ("00 00 01" + partial NAL) as a complete NAL.
static void flush_tail(uint8_t *buf, size_t *len, const char *why) {
  if (*len > 3) {
    size_t end = *len; if (buf[end - 1] == 0) end--;
    fprintf(stderr, "libkvm_shim: %s flush of a %zu-byte NAL\n", why, end - 3);
    on_nal(buf + 3, end - 3);
  }
  *len = 0;
  flush_pic();  // nothing more is coming for now: the picture is as complete as it will get
}

// ---------------------------------------------------------------- kvm_vin.toml -> ctl
static void poll_vin_toml(void) {
  static time_t seen; static double last_check;
  double t = now_s();
  if (t - last_check < 0.25) return;
  last_check = t;
  struct stat st;
  const char *path = getenv("KVM_SHIM_VIN_TOML") ? getenv("KVM_SHIM_VIN_TOML") : VIN_TOML;
  if (stat(path, &st) || st.st_mtime == seen) return;
  seen = st.st_mtime;
  FILE *f = fopen(path, "r");
  if (!f) return;
  char line[256];
  pthread_mutex_lock(&mu);
  while (fgets(line, sizeof line, f)) {
    char key[32], val[64];
    if (sscanf(line, " %31[a-z_] = %63s", key, val) != 2) continue;
    if (!strcmp(key, "mode")) ctl.codec_hevc = strstr(val, "h265") != NULL;
    else if (!strcmp(key, "fps")) { int v = atoi(val); ctl.fps = v > 0 ? v : 30; }  // 0 = "auto" in the toml
    else if (!strcmp(key, "bitrate")) ctl.bitrate = atoi(val);
    else if (!strcmp(key, "gop")) ctl.gop = atoi(val);
  }
  pthread_mutex_unlock(&mu);
  fclose(f);
  fprintf(stderr, "libkvm_shim: kvm_vin.toml -> %s fps=%d bitrate=%d gop=%d\n", ctl.codec_hevc ? "h265" : "h264", ctl.fps, ctl.bitrate, ctl.gop);
}

// ---------------------------------------------------------------- control channel
static int send_line(int fd, const char *s) {
  return send(fd, s, strlen(s), MSG_NOSIGNAL) < 0 ? -1 : 0;  // never SIGPIPE the server
}

// Send whatever changed since the last call. Returns -1 when the socket is gone.
static int send_control(int fd, int full) {
  typedef struct { int codec_hevc, fps, bitrate, gop, paused; } snap_t;
  static snap_t sent;
  char line[64];
  int idr;
  pthread_mutex_lock(&mu);
  snap_t c = { ctl.codec_hevc, ctl.fps, ctl.bitrate, ctl.gop, now_s() - last_read > PAUSE_AFTER_S };  // no viewer read a frame lately
  idr = ctl.idr; ctl.idr = 0;
  pthread_mutex_unlock(&mu);
  if (full) { if (send_line(fd, "ver 1\n")) return -1; }
#define SEND(cond, ...) if (full || (cond)) { snprintf(line, sizeof line, __VA_ARGS__); if (send_line(fd, line)) return -1; }
  SEND(c.codec_hevc != sent.codec_hevc, "codec %s\n", c.codec_hevc ? "h265" : "h264");
  SEND(c.fps != sent.fps, "fps %d\n", c.fps);
  SEND(c.bitrate != sent.bitrate, "bitrate %d\n", c.bitrate);
  SEND(c.gop != sent.gop, "gop %d\n", c.gop);
  SEND(c.paused != sent.paused, "%s\n", c.paused ? "pause" : "resume");
  if (idr) { if (send_line(fd, "idr\n")) return -1; }
#undef SEND
  sent = c;
  return 0;
}

// ---------------------------------------------------------------- stream reader
// Split Annex-B stream on 00 00 01 start codes. A NAL is complete when the next start code arrives;
// the client inserts access-unit delimiters so every frame is terminated promptly. The idle flush
// (200 ms) only catches the last frame before the client goes quiet; a flush
// mid-frame would corrupt it, which is why the default is far above any link stall.
static int feed(int fd, int ls) {  // returns a newly accepted fd to switch to, or -1
  uint8_t *buf = malloc(RDBUF);
  size_t len = 0, scan = 0, start = 0;  // scan: first offset not yet checked for a start code; start: body of the open NAL
  int have_start = 0;
  const int idle_ms = 200;
  struct pollfd pf[2] = { { fd, POLLIN, 0 }, { ls, POLLIN, 0 } };
  int next = -1;
  for (;;) {
    int pr = poll(pf, ls >= 0 ? 2 : 1, idle_ms);
    if (pr < 0) break;
    if (ls >= 0 && (pf[1].revents & POLLIN)) {  // a newer client: the old link is stale after a USB reset
      next = accept(ls, NULL, NULL);
      if (next >= 0) { fprintf(stderr, "libkvm_shim: newer client connected, replacing the current stream\n"); break; }
    }
    pr = pf[0].revents & POLLIN ? 1 : 0;
    // settings, pending control messages (including pause/resume, derived from the last viewer read)
    poll_vin_toml();
    if (send_control(fd, 0)) break;
    if (pr == 0) { flush_tail(buf, &len, "idle"); scan = have_start = 0; continue; }
    if (len == RDBUF) len = scan = have_start = 0;  // oversized NAL: drop
    ssize_t r = read(fd, buf + len, RDBUF - len);
    if (r <= 0) { flush_tail(buf, &len, "eof"); break; }  // peer closed or keepalive gave up
    len += r;
    size_t i = scan;
    while (i + 3 <= len) {
      const uint8_t *z = memchr(buf + i, 0, len - i - 2);  // a start code is a zero with two more bytes after it
      if (!z) { i = len - 2; break; }
      i = z - buf;
      if (buf[i + 1] == 0 && buf[i + 2] == 1) {
        if (have_start) {
          size_t end = i; if (end > start && buf[end - 1] == 0) end--;  // 4-byte start code
          on_nal(buf + start, end - start);
        }
        if (i + 3 < len && is_aud(buf[i + 3])) flush_pic();  // the delimiter's start: the picture is complete now, not when the delimiter is
        i += 3; start = i; have_start = 1;
      } else i++;
    }
    if (!have_start) { len = scan = 0; continue; }
    // keep the unfinished NAL, re-prefixed with a start code, for the next pass; only its new bytes get scanned
    memmove(buf + 3, buf + start, len - start);
    len = len - start + 3; scan = i - start + 3; start = 3;
    buf[0] = 0; buf[1] = 0; buf[2] = 1;
  }
  free(buf);
  return next;
}

static void keepalive(int fd) {  // notice a peer that vanished without a FIN (USB re-enumeration) within ~20 s
  int on = 1, idle = 5, intvl = 5, cnt = 3;
  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
#ifdef TCP_KEEPIDLE
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
#else
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof idle);  // macOS spelling, for the native self-check
#endif
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
}

static void *server(void *arg) {
  (void)arg;
  int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(5555), .sin_addr.s_addr = INADDR_ANY };
  if (bind(ls, (void *)&a, sizeof a) || listen(ls, 1)) { perror("libkvm_shim bind"); return NULL; }
  int fd = -1;
  for (;;) {
    if (fd < 0) { fd = accept(ls, NULL, NULL); if (fd < 0) continue; }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    keepalive(fd);
    fprintf(stderr, "libkvm_shim: stream connected\n");
    pthread_mutex_lock(&mu);
    drain_locked();
    free(vps.p); free(sps.p); free(pps.p); vps.p = sps.p = pps.p = NULL;
    ctl.idr = 0;
    net_connected = 1;
    pthread_mutex_unlock(&mu);
    int next = send_control(fd, 1) == 0 ? feed(fd, ls) : -1;
    close(fd);
    pthread_mutex_lock(&mu);
    net_connected = 0;
    drain_locked();
    pthread_mutex_unlock(&mu);
    if (next < 0) {  // only when no replacement is waiting: hand the viewers back to the stock pipeline
      if (real.close_video_stream) real.close_video_stream();  // stock pipeline re-reads the mode it missed
      if (real.force_idr) real.force_idr();                    // viewers switch back on a keyframe
    }
    fprintf(stderr, "libkvm_shim: stream disconnected\n");
    fd = next;
  }
}

// ---------------------------------------------------------------- exported API
void kvmv_init(uint8_t debug) {
  load_real();
  if (real.init) real.init(debug);   // last_read stays 0: nobody has read a frame yet, so a client connecting now hears "pause"
  poll_vin_toml();
  pthread_t th;
  pthread_create(&th, NULL, server, NULL);
  pthread_detach(th);
}

int kvmv_read_img(uint16_t w, uint16_t h, uint8_t type, uint16_t q_, uint8_t **out, uint32_t *n) {
  pthread_mutex_lock(&mu);
  last_read = now_s();                       // a viewer is reading: the feed loop's next pass says "resume"
  int net = net_connected;
  pthread_mutex_unlock(&mu);

  if (!net) return real.read_img ? real.read_img(w, h, type, q_, out, n) : IMG_NOT_EXIST;
  if (type != 1 && type != 5) return IMG_NOT_EXIST;  // same validation as the stock lib; it ignores which one

  // Block up to 500 ms for a NAL like the stock library does; the server polls at 120 Hz and logs every -1.
  struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_nsec += 500 * 1000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
  pthread_mutex_lock(&mu);
  int ci = cursor_locked(type);
  while (cons[ci].next >= seq_next) {
    if (pthread_cond_timedwait(&cv, &mu, &ts)) { pthread_mutex_unlock(&mu); return IMG_NOT_EXIST; }
  }
  if (seq_next - cons[ci].next > QCAP) cons[ci].next = latest_key_locked();  // fell behind: resync on a keyframe
  nal_t x = ring[cons[ci].next % QCAP];
  cons[ci].next++;
  pthread_mutex_unlock(&mu);
  *out = x.p; *n = x.n;
  static int last_logged = -1;
  if (x.code != last_logged && is_key(x.code)) {  // first keyframe of a new codec
    fprintf(stderr, "libkvm_shim: serving %s keyframe, %u bytes\n", x.code == 7 ? "h265" : "h264", x.n);
    last_logged = x.code;
  }
  return x.code;
}

int kvmv_set_fps(uint8_t f) { return real.set_fps ? real.set_fps(f) : 0; }
int kvmv_get_fps(void) { return real.get_fps ? real.get_fps() : ctl.fps; }
int kvmv_set_gop(uint16_t g) { return real.set_gop ? real.set_gop(g) : 0; }
int kvmv_set_bitrate(uint16_t b) { return real.set_bitrate ? real.set_bitrate(b) : 0; }
int kvmv_set_rate_control(uint8_t m) { return real.set_rate_control ? real.set_rate_control(m) : 0; }
int kvmv_force_idr(void) { pthread_mutex_lock(&mu); ctl.idr = 1; pthread_mutex_unlock(&mu); return real.force_idr ? real.force_idr() : 0; }
int kvmv_hdmi_control(uint8_t e) { return real.hdmi_control ? real.hdmi_control(e) : 0; }
void kvmv_close_video_stream(void) { if (!net_connected && real.close_video_stream) real.close_video_stream(); }  // stock close blocks ~3 s; skipped while the client streams, replayed on disconnect
void kvmv_deinit(void) { if (real.deinit) real.deinit(); }
int kvmv_read_audio(uint8_t **out, uint32_t *n) {
  if (!net_connected && real.read_audio) return real.read_audio(out, n);
  *out = NULL; *n = 0; return 0;  // no audio from the client: "no data", not an error, keeps the log quiet
}
int kvmv_reset_audio_capture(void) { return real.reset_audio_capture ? real.reset_audio_capture() : 0; }
int kvmv_audio_playback_open(uint32_t a) { return real.audio_playback_open ? real.audio_playback_open(a) : -1; }
int kvmv_audio_playback_close(void) { return real.audio_playback_close ? real.audio_playback_close() : 0; }
int kvmv_audio_playback_write_opus(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
  return real.audio_playback_write_opus ? real.audio_playback_write_opus(a, b, c, d, e) : -1;
}


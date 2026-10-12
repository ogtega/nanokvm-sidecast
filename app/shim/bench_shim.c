// Parse-path benchmark for the shim: feeds an Annex-B file through a socket in fixed chunks (the device
// reads about 160 KB per call), one consumer drains the ring, and the feed thread's own CPU time is
// reported. Run it natively and cross-built on the KVM before and after touching feed() or on_nal().
//   cc -O2 -o bench_shim bench_shim.c -lpthread
//   python3 -c 'open("big.h264","wb").write(open("test_vt.h264","rb").read()*480)'   # or a captured stream
//   ./bench_shim big.h264 163840
#include "libkvm_shim.c"
#include <assert.h>
#include <fcntl.h>
#include <sys/mman.h>
static size_t chunk;
static void *writer(void *arg) {
  int *a = arg; struct stat st; fstat(a[0], &st);
  uint8_t *m = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, a[0], 0);
  for (size_t off = 0; off < (size_t)st.st_size; ) {
    size_t n = (size_t)st.st_size - off < chunk ? (size_t)st.st_size - off : chunk;
    ssize_t w = write(a[1], m + off, n); if (w <= 0) break; off += w;
  }
  shutdown(a[1], SHUT_WR); return NULL;   // half-close: feed() sees EOF but can still send control lines
}
static long frames, keys; static int done;
static void *consumer(void *arg) {  // unpaced: with many tiny NALs the ring drops some, so frames may read low
  (void)arg; uint8_t *p; uint32_t n; int r;
  for (;;) { r = kvmv_read_img(0, 0, 5, 0, &p, &n); if (r >= 0) { frames++; keys += is_key(r); } else if (done) break; }
  return NULL;
}
static double cpu_s(void) { struct timespec t; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t); return t.tv_sec + t.tv_nsec / 1e9; }
int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: bench_shim <annexb file> <chunk bytes>\n"); return 2; }
  chunk = atoi(argv[2]);
  int sv[2]; assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  int a[2] = { open(argv[1], O_RDONLY), sv[1] }; assert(a[0] >= 0);
  net_connected = 1;
  pthread_t tw, tc; pthread_create(&tc, NULL, consumer, NULL); pthread_create(&tw, NULL, writer, a);
  double c0 = cpu_s(), w0 = now_s();
  feed(sv[0], -1);
  double c1 = cpu_s(), w1 = now_s();
  done = 1; pthread_join(tc, NULL); pthread_join(tw, NULL);
  struct stat st; fstat(a[0], &st);
  printf("chunk=%zu feed cpu %.1f ms wall %.1f ms (%.0f MB/s) frames=%ld keys=%ld\n", chunk, (c1 - c0) * 1e3, (w1 - w0) * 1e3, st.st_size / (w1 - w0) / 1e6, frames, keys);
  return 0;
}

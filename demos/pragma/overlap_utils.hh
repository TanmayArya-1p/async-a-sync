#pragma once

// The hash, timing, setup and report for demo_pragma_overlap.

#include "pragma_utils.hh"

#define OVERLAP_BYTES 4096

// How many times the hash mixes each 8-byte word. overlap_setup sizes it so
// hashing every file takes as long as reading them with blocking pread.
static int hash_rounds __attribute__((unused)) = 1;

// FNV-1a over 8-byte words, mixed hash_rounds times: costs compute, not
// memory access. noinline, so no caller sees hash_rounds as a constant:
// the rounds must cost the same while the hash is sized as after.
__attribute__((noinline)) static unsigned long hash_file(const unsigned char* p) {
  const unsigned long* w = (const unsigned long*)p;
  unsigned long h = 1469598103934665603UL;
  for (size_t i = 0; i < files.bytes / sizeof(*w); i++) {
    unsigned long x = w[i];
    for (int r = 0; r < hash_rounds; r++)
      h = (h ^ (x + (unsigned long)r)) * 1099511628211UL;
  }
  return h;
}

typedef unsigned long (*overlap_fn)(int n);

struct run {
  double ms;
  unsigned long hash;
  unsigned long waits; // io_uring_enter calls that slept
};

struct demo {
  int passes;
  int fixed_rounds;
  int cache_matters;
  unsigned long expect;
  struct run reads_only[MAX_PASSES];
  struct run hash_only[MAX_PASSES];
  struct run blocking[MAX_PASSES]; // A: read_then_hash
  struct run async[MAX_PASSES];    // B: async_then_hash
};

// The two halves of A, timed alone.
static inline unsigned long reads_only(int n) {
  for (int i = 0; i < n; i++)
    pread(files.fd[i], files.buf[i], files.bytes, 0);
  return 0;
}

static inline unsigned long hash_only(int n) {
  unsigned long h = 0;
  for (int i = 0; i < n; i++)
    h ^= hash_file(files.buf[i]);
  return h;
}

// Times one run over every file. hash_only needs the data in memory; every
// other run starts from empty buffers and a dropped cache.
static inline void time_run(struct run* r, overlap_fn fn) {
  if (fn == hash_only) {
    reads_only(files.n);
  } else {
    for (int i = 0; i < files.n; i++)
      memset(files.buf[i], 0, files.bytes);
    drop_cache();
  }
  // The counters also let the runtime see the last run's completions.
  struct counters start = counters_now();
  double t = now_ms();
  r->hash = fn(files.n);
  r->ms = now_ms() - t;
  r->waits = counters_since(start).waits;
}

// Sizes hash_rounds so hashing costs about as much as the blocking reads.
// Two points, because each word also costs something besides its rounds.
static inline void size_hash(struct demo* d) {
  struct run r;
  if (d->fixed_rounds) {
    hash_rounds = d->fixed_rounds;
  } else {
    double reads[3];
    for (int i = 0; i < 3; i++) {
      time_run(&r, reads_only);
      reads[i] = r.ms;
    }
    double reads_ms = median(reads, 3);
    hash_rounds = 16;
    time_run(&r, hash_only);
    double at16 = r.ms;
    hash_rounds = 64;
    time_run(&r, hash_only);
    double per_round = (r.ms - at16) / 48;
    double base = at16 - 16 * per_round;
    hash_rounds = (int)((reads_ms - base) / (per_round > 0 ? per_round : 1e-9) + 0.5);
    if (hash_rounds < 1)
      hash_rounds = 1;
  }
  time_run(&r, hash_only);
  d->expect = r.hash;
}

// Parses [dir] [files] [passes] [rounds], makes the files, sizes the hash,
// and times the reads and the hashing alone.
static inline struct demo setup(int argc, char** argv) {
  struct demo d = {0};
  int n = argc > 2 ? atoi(argv[2]) : 256;
  d.passes = argc > 3 ? atoi(argv[3]) : 5;
  d.fixed_rounds = argc > 4 ? atoi(argv[4]) : 0;
  if (n < 1 || n > MAX_FILES || d.passes < 1 || d.passes > MAX_PASSES ||
      d.fixed_rounds < 0) {
    fprintf(stderr, "usage: %s [dir] [files 1..%d] [passes 1..%d] [rounds]\n",
            argv[0], MAX_FILES, MAX_PASSES);
    exit(2);
  }

  title("overlap: the reads run while the program hashes", 0);
  printf("  Read %d cold files of %d bytes and hash each one, median of %d\n"
         "  passes. The hash is sized to take as long as the blocking reads.\n\n",
         n, OVERLAP_BYTES, d.passes);

  d.cache_matters = prepare_files(argc, argv, n, OVERLAP_BYTES);
  size_hash(&d);
  for (int pass = 0; pass < d.passes; pass++) {
    time_run(&d.reads_only[pass], reads_only);
    time_run(&d.hash_only[pass], hash_only);
  }
  return d;
}

static inline double run_median(const struct run* runs, int n) {
  double v[MAX_PASSES];
  for (int i = 0; i < n; i++)
    v[i] = runs[i].ms;
  return median(v, n);
}

static inline int report(struct demo* d) {
  static const char* const name[] = {
      "reads only, pread", "hashing only (data in memory)",
      "A  pread + hash, file by file", "B  async_pread all, then hash"};
  const struct run* way[] = {d->reads_only, d->hash_only, d->blocking, d->async};
  printf("  %-32s %8s\n", "run", "ms");
  for (int w = 0; w < 4; w++)
    printf("  %-32s %8.2f\n", name[w], run_median(way[w], d->passes));

  // B's sleeps waiting for a read: the most over any pass.
  unsigned long b_sleeps = 0;
  int hashes_ok = 1;
  for (int i = 0; i < d->passes; i++) {
    if (d->async[i].waits > b_sleeps)
      b_sleeps = d->async[i].waits;
    hashes_ok &= d->blocking[i].hash == d->expect && d->async[i].hash == d->expect;
  }

  double a = run_median(d->blocking, d->passes);
  double b = run_median(d->async, d->passes);
  printf("\n  => %.2fx faster than blocking; the hash loop waited on the "
         "device %lu times\n",
         ratio(a, b), b_sleeps);

  check("A and B hashed the same bytes as the reference", hashes_ok);
  check_bodies();
  if (d->cache_matters) {
    check("B beat A on an uncached device", b < a);
    check("B's hash loop never slept waiting for a read", b_sleeps == 0);
  } else {
    printf("  (timings not checked: dropping the cache added no device latency)\n");
  }

  remove_files();
  return finish();
}

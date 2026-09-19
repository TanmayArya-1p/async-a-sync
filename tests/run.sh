#!/bin/sh
#
# run.sh -- build everything and run the whole suite.
#
# Stages, in order:
#
#   stage2b  kernel behaviour: how an io_uring ring can and cannot be mapped
#            (plain C; establishes why the runtime is shaped the way it is)
#   stage2c  GC stability: Fil-C GC memory must not move, because the kernel is
#            handed a pointer to it (Fil-C)
#   stage2   lazy resolution: submission never blocks, resolution happens on
#            first genuine access (Fil-C)
#   stage6b  kernel probe: whether an fd can be chained to a not-yet-open file
#            (plain C; documents a kernel limitation the runtime is built around)
#   stage3   dependencies: declared effect sets, capability-range disjointness,
#            DAG construction and execution (Fil-C)
#   stage5   serialization tokens (async-a-sync.pdf) vs effect sets (Fil-C)
#   stage6   descriptor provenance: operations against a not-yet-open fd (Fil-C)
#   stage7   many small reads: kernel entries saved by batching, vs wall clock
#   stage8   the latency regime: blocking vs explicit wait vs implicit (Fil-C)
#   stage9   device parallelism probe: how much of it the kernel can sustain
#            (plain C, pthreads)
#
# Usage: ./tests/run.sh
#        FILC_ROOT=/path/to/filc-dist ./tests/run.sh

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)

FILC_ROOT=${FILC_ROOT:-$REPO/vendor/filc-0.685-linux-x86_64}
FILCC=${FILCC:-$FILC_ROOT/build/bin/filcc}
HOST_CC=${HOST_CC:-cc}
OUT=${OUT:-$REPO/build/tests}

mkdir -p "$OUT"

if [ ! -x "$FILCC" ]; then
  echo "run.sh: filcc not found at $FILCC (set FILC_ROOT)" >&2
  exit 1
fi

echo "### building the runtime"
"$REPO/runtime/build.sh"

FAILED=0
PASSED=0

run_filc_test() {
  name=$1
  echo
  echo "### $name (Fil-C)"
  if "$FILCC" -O2 -static -I"$REPO/runtime/src" -L"$REPO/runtime/build/lib" \
       -o "$OUT/$name" "$HERE/$name.c"; then
    if "$OUT/$name"; then
      PASSED=$((PASSED + 1))
    else
      echo "!!! $name exited non-zero"
      FAILED=$((FAILED + 1))
    fi
  else
    echo "!!! $name failed to build"
    FAILED=$((FAILED + 1))
  fi
}

run_host_test() {
  name=$1
  echo
  echo "### $name (plain C)"
  if "$HOST_CC" -O2 -o "$OUT/$name" "$HERE/$name.c"; then
    if "$OUT/$name"; then
      PASSED=$((PASSED + 1))
    else
      echo "!!! $name exited non-zero"
      FAILED=$((FAILED + 1))
    fi
  else
    echo "!!! $name failed to build"
    FAILED=$((FAILED + 1))
  fi
}

# Same, but a probe that needs threads to measure device parallelism. It takes the
# directory to put its payload in, for the same real-filesystem reason as above.
run_host_test_pthread() {
  name=$1
  echo
  echo "### $name (plain C, pthreads)"
  if "$HOST_CC" -O2 -pthread -o "$OUT/$name" "$HERE/$name.c"; then
    if "$OUT/$name" "$OUT"; then
      PASSED=$((PASSED + 1))
    else
      echo "!!! $name exited non-zero"
      FAILED=$((FAILED + 1))
    fi
  else
    echo "!!! $name failed to build"
    FAILED=$((FAILED + 1))
  fi
}

run_host_test stage2b_mmap_probe
run_host_test stage6b_fd_chain_probe
run_filc_test stage2c_gc_pin_probe
run_filc_test stage2_lazy_resolution
run_filc_test stage3_dependency
run_filc_test stage5_trackers
run_filc_test stage6_fd_provenance
run_filc_test stage7_throughput

# stage4, stage8, demo_plain_io and demo_wordcount all need the *patched*
# compiler, because what they demonstrate is the hook it inserts. Built with the
# stock compiler they would still link and run, and would silently do nothing of
# the sort -- stage8 and demo_wordcount refuse at compile time for that reason.
# Skipped (not failed) when that compiler has not been built, since building it is
# a separate, expensive step.
PATCHED_CC=$REPO/vendor/fil-c-src/build/bin/filcc
PATCHED_READY=0
if [ -x "$PATCHED_CC" ] && grep -q "filc_resolve_pending" \
     "$REPO/vendor/fil-c-src/llvm/lib/Transforms/Instrumentation/FilPizlonator.cpp" 2>/dev/null; then
  PATCHED_READY=1
fi

# run_patched <name> <source> [program args...]
run_patched() {
  name=$1
  src=$2
  shift 2
  echo
  echo "### $name (patched compiler)"
  if "$PATCHED_CC" -O2 -static -DFASYNC_COMPILER_INSERTS_CHECKS \
       -I"$REPO/runtime/src" -L"$REPO/runtime/build/lib" \
       -o "$OUT/$name" "$src"; then
    if "$OUT/$name" "$@"; then
      PASSED=$((PASSED + 1))
    else
      echo "!!! $name exited non-zero"
      FAILED=$((FAILED + 1))
    fi
  else
    echo "!!! $name failed to build"
    FAILED=$((FAILED + 1))
  fi
}

# The payloads go under $OUT so they land on a real filesystem: the timing in
# stage8 and demo_wordcount is only meaningful where a read costs a device round
# trip, and /tmp is usually tmpfs. Both programs detect the no-latency case and
# say so rather than quoting a ratio they cannot support.
if [ "$PATCHED_READY" -eq 1 ]; then
  run_patched stage4_compiler_hook "$HERE/stage4_compiler_hook.c"
  run_patched stage8_latency "$HERE/stage8_latency.c" "$OUT"
  run_patched demo_plain_io "$REPO/demos/demo_plain_io.c"
  run_patched demo_wordcount "$REPO/demos/demo_wordcount.c" "$OUT"
else
  echo
  echo "### stage4_compiler_hook, stage8_latency, demo_plain_io, demo_wordcount:"
  echo "    SKIPPED (patched compiler not built)"
  echo "    build it with: ./compiler/build.sh"
fi

# Not a runtime test: it measures how much parallelism the device underneath these
# benchmarks can actually sustain, so their numbers can be read against it.
run_host_test_pthread stage9_device_parallelism

echo
echo "### demo_async_io (Fil-C, showcase)"
if "$FILCC" -O2 -static -I"$REPO/runtime/src" -L"$REPO/runtime/build/lib" \
     -o "$OUT/demo_async_io" "$REPO/demos/demo_async_io.c"; then
  "$OUT/demo_async_io" || FAILED=$((FAILED + 1))
else
  echo "!!! demo failed to build"
  FAILED=$((FAILED + 1))
fi

echo
echo "==============================================="
echo "tests passed: $PASSED   failed: $FAILED"
echo "==============================================="
[ "$FAILED" -eq 0 ]

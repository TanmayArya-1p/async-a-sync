#!/bin/sh
# run.sh -- build everything and run the whole suite.
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
run_filc_test stage2_lazy_submit
run_filc_test stage_token_ordering
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
if [ -x "$PATCHED_CC" ]; then
  # The source-built clang looks for its Fil-C runtime at
  # <binary>/../../../pizfix (i.e. $REPO/vendor/pizfix). Point that at the
  # distribution's pizfix so the patched compiler can find crt1.o, yolort, etc.
  PATCHED_PIZFIX=$(cd "$(dirname "$PATCHED_CC")/../../.." && pwd)/pizfix
  if [ ! -e "$PATCHED_PIZFIX" ]; then
    ln -sfn "$FILC_ROOT/pizfix" "$PATCHED_PIZFIX"
  fi
fi
if [ -x "$PATCHED_CC" ] && grep -q "filc_resolve_pending" \
     "$REPO/vendor/fil-c-src/llvm/lib/Transforms/Instrumentation/FilPizlonator.cpp" 2>/dev/null; then
  PATCHED_READY=1
fi

# run_patched <name> <source> [program args...]
# Set RUN_PATCHED_FLAGS to add extra -D flags to the build.
run_patched() {
  name=$1
  src=$2
  shift 2
  echo
  echo "### $name (patched compiler)"
  # shellcheck disable=SC2086
  if "$PATCHED_CC" -O2 -static -DFASYNC_COMPILER_INSERTS_CHECKS \
       $RUN_PATCHED_FLAGS \
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
  RUN_PATCHED_FLAGS="-DFASYNC_IMPLICIT" run_patched demo_plain_io \
    "$REPO/demos/demo_plain_io.c" "$OUT"
  RUN_PATCHED_FLAGS="-DFASYNC_IMPLICIT" run_patched demo_async_io \
    "$REPO/demos/demo_async_io.c" "$OUT"
  RUN_PATCHED_FLAGS="-DFASYNC_IMPLICIT" run_patched demo_provenance \
    "$REPO/demos/demo_provenance.c" "$OUT"
  RUN_PATCHED_FLAGS="-DFASYNC_IMPLICIT" run_patched demo_wordcount \
    "$REPO/demos/demo_wordcount.c" "$OUT"

  # The two-backend comparison, as a standalone script: the same word-count
  # source built one way with plain Fil-C and one way with the patched
  # compiler + io_uring, run on a real filesystem, and reported as two
  # timings plus a ratio.
  echo
  echo "### wordcount: the same code, sync and implicit (run_wordcount.sh)"
  if "$REPO/demos/run_wordcount.sh" "$OUT"; then
    PASSED=$((PASSED + 1))
  else
    echo "!!! run_wordcount.sh exited non-zero"
    FAILED=$((FAILED + 1))
  fi
else
  echo
  echo "### stage4_compiler_hook, stage8_latency, demo_plain_io, demo_async_io,"
  echo "    demo_provenance, demo_wordcount, run_wordcount.sh:"
  echo "    SKIPPED (patched compiler not built)"
  echo "    build it with: ./compiler/build.sh"
fi

# Not a runtime test: it measures how much parallelism the device underneath these
# benchmarks can actually sustain, so their numbers can be read against it.
run_host_test_pthread stage9_device_parallelism

echo

echo
echo "==============================================="
echo "tests passed: $PASSED   failed: $FAILED"
echo "==============================================="
[ "$FAILED" -eq 0 ]

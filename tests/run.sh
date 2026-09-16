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

run_host_test stage2b_mmap_probe
run_host_test stage6b_fd_chain_probe
run_filc_test stage2c_gc_pin_probe
run_filc_test stage2_lazy_resolution
run_filc_test stage3_dependency
run_filc_test stage5_trackers
run_filc_test stage6_fd_provenance

# stage4 needs the *patched* compiler: it is the test that the pass inserts the
# resolution hook automatically. Skipped (not failed) when that compiler has not
# been built, since building it is a separate, expensive step.
PATCHED_CC=$REPO/vendor/fil-c-src/build/bin/filcc
if [ -x "$PATCHED_CC" ] && grep -q "filc_resolve_pending" \
     "$REPO/vendor/fil-c-src/llvm/lib/Transforms/Instrumentation/FilPizlonator.cpp" 2>/dev/null; then
  echo
  echo "### stage4_compiler_hook (patched compiler)"
  if "$PATCHED_CC" -O2 -static -DFASYNC_COMPILER_INSERTS_CHECKS \
       -I"$REPO/runtime/src" -L"$REPO/runtime/build/lib" \
       -o "$OUT/stage4_compiler_hook" "$HERE/stage4_compiler_hook.c"; then
    if "$OUT/stage4_compiler_hook"; then
      PASSED=$((PASSED + 1))
    else
      FAILED=$((FAILED + 1))
    fi
  else
    echo "!!! stage4 failed to build"
    FAILED=$((FAILED + 1))
  fi
else
  echo
  echo "### stage4_compiler_hook: SKIPPED (patched compiler not built)"
  echo "    build it with: ./compiler/build.sh"
fi

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

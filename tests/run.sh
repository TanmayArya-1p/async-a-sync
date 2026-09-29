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

"$HERE/check_forwarders.sh"
"$HERE/check_dependencies.sh"
"$HERE/check_dependency_options.sh"
"$HERE/check_callsite_pragma.sh"

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
  shift
  echo
  echo "### $name (Fil-C)"
  if "$FILCC" -O2 -static -I"$REPO/runtime/src" -L"$REPO/runtime/build/lib" \
       -o "$OUT/$name" "$HERE/$name.c" -lpizlo -lc; then
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
run_filc_test t_pending_registry
run_filc_test t_backend_io_uring "$OUT"

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
  # <binary>/../../pizfix (i.e. $REPO/vendor/fil-c-src/pizfix). Point that at the
  # distribution's pizfix so the patched compiler can find crt1.o, yolort, etc.
  PATCHED_PIZFIX=$(cd "$(dirname "$PATCHED_CC")/../.." && pwd)/pizfix
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
  if "$PATCHED_CC" -O2 -static -Werror=pragma-clang-attribute \
       -DFASYNC_COMPILER_INSERTS_CHECKS \
       $RUN_PATCHED_FLAGS \
       -I"$REPO/runtime/src" -L"$REPO/runtime/build/lib" \
       -o "$OUT/$name" "$src" -lpizlo -lc; then
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

# Compile a caller and annotated implementation in separate translation units,
# then inspect the archive and final executable before running the program.
run_patched_linked() {
  echo
  echo "### t_linked_async (patched compiler, two translation units)"
  if "$PATCHED_CC" -O2 -static -DFASYNC_COMPILER_INSERTS_CHECKS \
       -I"$REPO/runtime/src" -L"$REPO/runtime/build/lib" \
       -o "$OUT/t_linked_async" \
       "$HERE/t_linked_async_main.c" "$HERE/t_linked_async_def.c" \
       -lpizlo -lc; then
    if "$HERE/check_linkage.sh" "$REPO/runtime/build/lib/libpizlo.a" \
         "$OUT/t_linked_async" && "$OUT/t_linked_async" "$OUT"; then
      PASSED=$((PASSED + 1))
    else
      echo "!!! t_linked_async linkage or execution failed"
      FAILED=$((FAILED + 1))
    fi
  else
    echo "!!! t_linked_async failed to link"
    FAILED=$((FAILED + 1))
  fi
}

# run_patched_neg <name> <source>: builds like run_patched but expects the
# program to be rejected at startup (runtime-side op validation). Passes only
# when it dies with the validator's rejection message.
run_patched_neg() {
  name=$1
  src=$2
  shift 2
  echo
  echo "### $name (patched compiler, expects runtime rejection)"
  # shellcheck disable=SC2086
  if "$PATCHED_CC" -O2 -static -DFASYNC_COMPILER_INSERTS_CHECKS \
       $RUN_PATCHED_FLAGS \
       -I"$REPO/runtime/src" -L"$REPO/runtime/build/lib" \
       -o "$OUT/$name" "$src" -lpizlo -lc; then
    if err_out=$("$OUT/$name" 2>&1); then
      echo "!!! $name exited 0; runtime should have rejected the op"
      FAILED=$((FAILED + 1))
    elif echo "$err_out" | grep -q "cannot be registered on this runtime"; then
      PASSED=$((PASSED + 1))
    else
      echo "!!! $name died without the validator rejecting it: $(echo "$err_out" | head -1)"
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

  # The pragma-async dependency demos. One file per scenario, no shared helper:
  # each is self-contained so a reader can open exactly one of them. They assert
  # ordering, not timing, so they are cheap; the timing harness is
  # run_wordcount.sh below.
  run_patched demo_pragma_nodeps "$REPO/demos/demo_pragma_nodeps.c" "$OUT"
  run_patched demo_pragma_ptrdeps "$REPO/demos/demo_pragma_ptrdeps.c" "$OUT"
  run_patched demo_pragma_mixdeps "$REPO/demos/demo_pragma_mixdeps.c" "$OUT"

  # The pragma-async interface tests. t_pragma_alloc uses only the allocator
  # functions (no annotations), so it compiles and links with the stock filcc
  # against the arena object alone; t_pragma_ignore needs the patched compiler
  # to rewrite its annotated call site into filc_async_submit.
  run_filc_test t_pragma_alloc
  run_patched t_pragma_ignore "$HERE/t_pragma_ignore.c"
  run_patched t_pragma_markpending "$HERE/t_pragma_markpending.c"
  run_patched_linked
  run_patched t_pragma_io_uring "$HERE/t_pragma_io_uring.c" "$OUT"
  run_patched t_pragma_dependencies "$HERE/t_pragma_dependencies.c" "$OUT"
  # Regression: every annotated call site must submit even when the call is
  # foldable, i.e. the pass has to rewrite call sites before the Fil-C inliner
  # gets a chance to remove them.
  run_patched t_pragma_repeat_read "$HERE/t_pragma_repeat_read.c"
  # Regression: an op on a bad fd must report -EBADF, not hang (the ring can
  # steal the number of a closed fd from io_uring_setup). Self-bounding via
  # alarm() so a regression fails the test instead of wedging the suite.
  run_patched t_pragma_error_path "$HERE/t_pragma_error_path.c"
  # Negative control: an unknown op= is accepted by the pass and rejected by
  # the runtime's startup validator (the runtime is the authority).
  run_patched_neg t_pragma_unknownop "$HERE/t_pragma_unknownop.c"
  # The same unknown op, but with a permissive validator installed from a
  # priority-101 ctor. Proves the override hook is reachable and that it only
  # downgrades the abort to a per-task EINVAL.
  run_patched t_pragma_custom_validator "$HERE/t_pragma_custom_validator.c"

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
  echo "    demo_provenance, demo_wordcount, demo_pragma_nodeps,"
  echo "    demo_pragma_ptrdeps, demo_pragma_mixdeps, t_linked_async,"
  echo "    t_pragma_io_uring, t_pragma_dependencies, t_pragma_custom_validator,"
  echo "    run_wordcount.sh:"
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

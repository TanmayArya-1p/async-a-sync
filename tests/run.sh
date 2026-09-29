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
SKIPPED=0

# Warnings are reported, not fatal: the suite should still run on toolchains
# that warn about things ours do not.
WARN="-Wall -Wextra"

skip() {
  echo
  echo "### $1: SKIPPED ($2)"
  SKIPPED=$((SKIPPED + 1))
}

# run_check <script>: a host-side check script, counted like any test. Exit
# status 77 means a prerequisite is missing (it says which), so it is skipped.
run_check() {
  name=$1
  echo
  echo "### $name (host check)"
  status=0
  "$HERE/$name.sh" || status=$?
  if [ "$status" -eq 0 ]; then
    PASSED=$((PASSED + 1))
  elif [ "$status" -eq 77 ]; then
    skip "$name" "prerequisite missing"
  else
    echo "!!! $name exited $status"
    FAILED=$((FAILED + 1))
  fi
}

run_check check_forwarders
run_check check_dependencies
run_check check_dependency_options
run_check check_callsite_pragma

# Most of the suite submits real requests, so it needs a working io_uring.
# Without one those tests are skipped, not failed, so the ones that can run
# still say something. See tests/probe_io_uring.c for why it goes missing.
IO_URING=0
if "$HOST_CC" -O2 -o "$OUT/probe_io_uring" "$HERE/probe_io_uring.c" &&
   "$OUT/probe_io_uring"; then
  IO_URING=1
else
  echo
  echo "!!! io_uring is not available to this process; tests that need it are"
  echo "!!! skipped. In Docker, try --security-opt seccomp=unconfined. Under"
  echo "!!! x86-64 emulation (e.g. Rosetta on Apple silicon) it cannot work."
fi

# needs_io_uring <runner> <name> [args...]: run the test, or skip it without
# io_uring.
needs_io_uring() {
  if [ "$IO_URING" -eq 1 ]; then
    "$@"
  else
    skip "$2" "needs io_uring"
  fi
}

run_filc_test() {
  name=$1
  shift
  echo
  echo "### $name (Fil-C)"
  # shellcheck disable=SC2086
  if "$FILCC" -O2 -static $WARN -I"$REPO/runtime/src" -L"$REPO/runtime/build/lib" \
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

# run_filc_neg <name> <message> [args...]: like run_filc_test, but the program
# must stop with <message> on stderr; exiting normally or any other way fails.
run_filc_neg() {
  name=$1
  message=$2
  shift 2
  echo
  echo "### $name (Fil-C, expects the runtime to stop it)"
  # shellcheck disable=SC2086
  if "$FILCC" -O2 -static $WARN -I"$REPO/runtime/src" -L"$REPO/runtime/build/lib" \
       -o "$OUT/$name" "$HERE/$name.c" -lpizlo -lc; then
    # no core file for the expected abort
    if err_out=$(ulimit -c 0; "$OUT/$name" "$@" 2>&1); then
      echo "!!! $name exited 0; the runtime should have stopped it"
      FAILED=$((FAILED + 1))
    elif echo "$err_out" | grep -qF "$message"; then
      PASSED=$((PASSED + 1))
    else
      echo "!!! $name died without the expected message: $(echo "$err_out" | tail -n 1)"
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
  # shellcheck disable=SC2086
  if "$HOST_CC" -O2 $WARN -o "$OUT/$name" "$HERE/$name.c"; then
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
  # shellcheck disable=SC2086
  if "$HOST_CC" -O2 $WARN -pthread -o "$OUT/$name" "$HERE/$name.c"; then
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
needs_io_uring run_host_test stage6b_fd_chain_probe
run_filc_test stage2c_gc_pin_probe
needs_io_uring run_filc_test stage2_lazy_resolution
needs_io_uring run_filc_test stage2_lazy_submit
needs_io_uring run_filc_test stage_token_ordering
needs_io_uring run_filc_test stage3_dependency
needs_io_uring run_filc_test stage5_trackers
needs_io_uring run_filc_test stage6_fd_provenance
needs_io_uring run_filc_test stage7_throughput
run_filc_test t_pending_registry
run_filc_test t_dag_submit_failure
# Allocator interface only (no annotations), so the stock filcc builds it.
run_filc_test t_pragma_alloc
needs_io_uring run_filc_test t_backend_io_uring "$OUT"
needs_io_uring run_filc_test t_pending_open_failure "$OUT"
needs_io_uring run_filc_test t_openat_pending_path "$OUT"
needs_io_uring run_filc_neg t_thread_owner \
  "does not own the io_uring ring" "$OUT"

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
  if "$PATCHED_CC" -O2 -static $WARN -Werror=pragma-clang-attribute \
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

# run_patched_linked <name> [flags...]: compile a caller and its implementation
# in separate translation units, then inspect the archive and final executable
# before running the program. The flags go to both units.
run_patched_linked() {
  name=$1
  shift
  echo
  echo "### $name (patched compiler, two translation units)"
  # shellcheck disable=SC2086
  if "$PATCHED_CC" -O2 -static $WARN -Werror=pragma-clang-attribute \
       -DFASYNC_COMPILER_INSERTS_CHECKS "$@" \
       -I"$REPO/runtime/src" -L"$REPO/runtime/build/lib" \
       -o "$OUT/$name" \
       "$HERE/t_linked_async_main.c" "$HERE/t_linked_async_def.c" \
       -lpizlo -lc; then
    if "$HERE/check_linkage.sh" "$REPO/runtime/build/lib/libpizlo.a" \
         "$OUT/$name" && "$OUT/$name" "$OUT"; then
      PASSED=$((PASSED + 1))
    else
      echo "!!! $name linkage or execution failed"
      FAILED=$((FAILED + 1))
    fi
  else
    echo "!!! $name failed to link"
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
  if "$PATCHED_CC" -O2 -static $WARN -Werror=pragma-clang-attribute \
       -DFASYNC_COMPILER_INSERTS_CHECKS \
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
  needs_io_uring run_patched stage4_compiler_hook "$HERE/stage4_compiler_hook.c"
  needs_io_uring run_patched t_hook_memo "$HERE/t_hook_memo.c" "$OUT"
  needs_io_uring run_patched stage8_latency "$HERE/stage8_latency.c" "$OUT"
  RUN_PATCHED_FLAGS="-DFASYNC_IMPLICIT" needs_io_uring run_patched \
    demo_plain_io "$REPO/demos/demo_plain_io.c" "$OUT"
  RUN_PATCHED_FLAGS="-DFASYNC_IMPLICIT" needs_io_uring run_patched \
    demo_async_io "$REPO/demos/demo_async_io.c" "$OUT"
  RUN_PATCHED_FLAGS="-DFASYNC_IMPLICIT" needs_io_uring run_patched \
    demo_provenance "$REPO/demos/demo_provenance.c" "$OUT"
  RUN_PATCHED_FLAGS="-DFASYNC_IMPLICIT" needs_io_uring run_patched \
    demo_wordcount "$REPO/demos/demo_wordcount.c" "$OUT"

  # The pragma-async interface tests: the patched compiler rewrites their
  # annotated call sites into filc_async_submit.
  run_patched t_pragma_ignore "$HERE/t_pragma_ignore.c"
  run_patched t_pragma_markpending "$HERE/t_pragma_markpending.c"
  needs_io_uring run_patched t_pragma_many_calls "$HERE/t_pragma_many_calls.c"
  needs_io_uring run_patched_linked t_linked_async
  needs_io_uring run_patched_linked t_linked_async_annotated_def \
    -DLINKED_ANNOTATE_DEF
  needs_io_uring run_patched t_pragma_io_uring "$HERE/t_pragma_io_uring.c" "$OUT"
  needs_io_uring run_patched t_pragma_dependencies "$HERE/t_pragma_dependencies.c" "$OUT"
  needs_io_uring run_patched t_pragma_same_tu_lazy "$HERE/t_pragma_same_tu_lazy.c" "$OUT"
  needs_io_uring run_patched t_pragma_lazy_many "$HERE/t_pragma_lazy_many.c" "$OUT"
  needs_io_uring run_patched t_pragma_reuse_lazy "$HERE/t_pragma_reuse_lazy.c" "$OUT"
  needs_io_uring run_patched t_thread_compute "$HERE/t_thread_compute.c" "$OUT"
  # Negative control: an unknown op= is accepted by the pass and rejected by
  # the runtime's startup validator (the runtime is the authority).
  run_patched_neg t_pragma_unknownop "$HERE/t_pragma_unknownop.c"

  # The two-backend comparison, as a standalone script: the same word-count
  # source built one way with plain Fil-C and one way with the patched
  # compiler + io_uring, run on a real filesystem, and reported as two
  # timings plus a ratio.
  if [ "$IO_URING" -eq 1 ]; then
    echo
    echo "### wordcount: the same code, sync and implicit (run_wordcount.sh)"
    if "$REPO/demos/run_wordcount.sh" "$OUT"; then
      PASSED=$((PASSED + 1))
    else
      echo "!!! run_wordcount.sh exited non-zero"
      FAILED=$((FAILED + 1))
    fi
  else
    skip run_wordcount.sh "needs io_uring"
  fi

  # opt-level tests of the FilAsync pass itself (descriptor emission, call
  # rewriting, annotation erasure). They need opt, a host clang and cmake.
  if [ -x "$REPO/vendor/fil-c-src/build/bin/opt" ]; then
    for script in opt_annotate opt_annotate_test; do
      echo
      echo "### $script.sh (FilAsync pass under opt)"
      if "$REPO/compiler/dev/$script.sh"; then
        PASSED=$((PASSED + 1))
      else
        echo "!!! $script.sh exited non-zero"
        FAILED=$((FAILED + 1))
      fi
    done
  else
    skip "compiler/dev pass tests" "opt not built; run ./compiler/build.sh"
  fi
else
  skip "stage4, stage8, the demos, the t_pragma_* and FilAsync pass tests" \
    "patched compiler not built; run ./compiler/build.sh"
fi

# Not a runtime test: it measures how much parallelism the device underneath these
# benchmarks can actually sustain, so their numbers can be read against it.
run_host_test_pthread stage9_device_parallelism

echo
echo "==============================================="
echo "tests passed: $PASSED   failed: $FAILED   skipped: $SKIPPED"
echo "==============================================="
[ "$FAILED" -eq 0 ]

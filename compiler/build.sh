#!/bin/sh
#
# build.sh -- build a patched Fil-C clang that inserts the async-resolution hook.
#
# WHAT THIS DOES
# --------------
# Applies compiler/patches/0001-FilPizlonator-resolve-pending.patch to
# FilPizlonator.cpp and builds clang, producing a compiler that emits a call to
# filc_resolve_pending() alongside the capability check it already emits for
# every access through an escaping pointer.
#
# Until this binary exists, programs have to mark the same points by hand with
# the FASYNC_ACCESS() macro (see runtime/src/fasync.h). Once it exists, the macro
# becomes a no-op: build with -DFASYNC_COMPILER_INSERTS_CHECKS and the compiler
# supplies the calls. The runtime does not change either way.
#
# WHY THIS IS EXPENSIVE, AND WHY IT IS SEPARATE
# ---------------------------------------------
# Fil-C's pass is one large LLVM pass inside the compiler, so the hook cannot be
# added from outside: the distribution ships no libLLVM to link an out-of-tree
# plugin against, which was checked. So this needs a full clang build.
#
# Fil-C's own build uses RelWithDebInfo with assertions on, which is neither the
# cheapest nor the smallest configuration. The settings below are the cheap ones:
# Release, no assertions, X86 only. Even so expect a long build and a large
# build directory -- check your free space first, because an LLVM build that runs
# out of disk late is a bad afternoon.
#
# It also matters how much RAM you have. A parallel LLVM build uses roughly
# 1-2 GiB per compile job, so a machine with little free memory has to lower
# JOBS, and the build gets correspondingly longer.
#
# Usage:
#   JOBS=2 ./build.sh              # conservative
#   BUILD_TYPE=RelWithDebInfo ./build.sh
#
# Prerequisites: cmake, ninja, a host clang, and a checkout of the Fil-C sources
# (see ../runtime/build.sh for the clone command). Put it at vendor/fil-c-src or
# point FILC_SRC at it.

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)

FILC_SRC=${FILC_SRC:-$REPO/vendor/fil-c-src}
BUILD_DIR=${BUILD_DIR:-$FILC_SRC/build}
JOBS=${JOBS:-$(nproc)}
BUILD_TYPE=${BUILD_TYPE:-Release}

PATCH=$HERE/patches/0001-FilPizlonator-resolve-pending.patch
PASS=$FILC_SRC/llvm/lib/Transforms/Instrumentation/FilPizlonator.cpp

if [ ! -f "$PASS" ]; then
  echo "build.sh: cannot find $PASS" >&2
  echo "          set FILC_SRC to a Fil-C source checkout" >&2
  exit 1
fi

# ---------------------------------------------------------------------
# 1. Apply the patch, idempotently.
# ---------------------------------------------------------------------
if grep -q "ResolvePending" "$PASS"; then
  echo "== FilPizlonator patch already applied"
else
  echo "== applying $PATCH"
  ( cd "$FILC_SRC" && git apply "$PATCH" )
fi

# ---------------------------------------------------------------------
# 2. Report the resource situation honestly before starting.
# ---------------------------------------------------------------------
AVAIL_KB=$(df -Pk "$FILC_SRC" | awk 'NR==2 {print $4}')
AVAIL_GB=$((AVAIL_KB / 1024 / 1024))
echo "== free space at $FILC_SRC: ${AVAIL_GB} GiB"
if [ "$AVAIL_GB" -lt 20 ]; then
  echo "   WARNING: a clang build wants roughly 10-20 GiB in Release, and more" >&2
  echo "            with assertions. This may not fit." >&2
fi
echo "== compile jobs: $JOBS"

# ---------------------------------------------------------------------
# 3. Configure and build.
# ---------------------------------------------------------------------
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  echo "== configuring"
  cmake -S "$FILC_SRC/llvm" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DLLVM_ENABLE_PROJECTS=clang \
    -DLLVM_ENABLE_LLD=ON \
    -DLLVM_TARGETS_TO_BUILD=X86 \
    -DLLVM_ENABLE_ASSERTIONS=OFF
fi

echo "== building clang (this is the long part)"
ninja -C "$BUILD_DIR" -j "$JOBS" clang

echo "== done"
echo "   $BUILD_DIR/bin/clang"
echo
echo "Use it to link against a runtime built with the io_uring extension:"
echo "   $BUILD_DIR/bin/filcc -static -DFASYNC_COMPILER_INSERTS_CHECKS \\"
echo "     -I$REPO/runtime/src -L$REPO/runtime/build/lib ..."
echo
echo "NOTE: this build itself needs the pizfix runtime from a Fil-C distribution."
echo "See compiler/README.md for the full sequence and the known gaps."

#!/bin/sh
# build a patched Fil-C clang that inserts the async-resolution hook.
# Usage:
#   ./build.sh
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
JOBS=${JOBS:-$(( $(nproc) > 12 ? 12 : $(nproc) ))}
BUILD_TYPE=${BUILD_TYPE:-Release}

if [ ! -d "$FILC_SRC/llvm" ]; then
  echo "build.sh: cannot find $FILC_SRC/llvm" >&2
  echo "          set FILC_SRC to a Fil-C source checkout" >&2
  exit 1
fi

# ---------------------------------------------------------------------
# 1. Install our upstream overrides, idempotently.
#
# Every file under upstream-overrides mirrors a file inside the Fil-C
# source checkout; copy each one over the fetched checkout so the build
# below carries them. This is how we patch generated sources (CMake
# lists, clang BackendUtil, the FilPizlonator pass) without forking the
# whole tree. The diff -q guard keeps repeated runs cheap and idempotent.
# ---------------------------------------------------------------------
for f in $(cd "$HERE/upstream-overrides" && LC_ALL=C find . -type f); do
  rel=${f#./}
  dst="$FILC_SRC/$rel"
  if diff -q "$HERE/upstream-overrides/$f" "$dst" >/dev/null 2>&1; then
    echo "== already installed: $rel"
  else
    echo "== installing: $rel"
    mkdir -p "$(dirname "$dst")"
    cp "$HERE/upstream-overrides/$f" "$dst"
  fi
done

# Upstream's Release build omits AllocaSlices::AI, but its verbose log still
# refers to that field. Apply the one-line fix without rewriting other sources.
SROA_PATCH=$HERE/upstream-patches/sroa-release-verbose.patch
if git -C "$FILC_SRC" apply --check "$SROA_PATCH" 2>/dev/null; then
  echo "== installing: llvm/lib/Transforms/Scalar/SROA.cpp"
  git -C "$FILC_SRC" apply "$SROA_PATCH"
elif git -C "$FILC_SRC" apply --reverse --check "$SROA_PATCH" 2>/dev/null; then
  echo "== already installed: llvm/lib/Transforms/Scalar/SROA.cpp"
else
  echo "build.sh: SROA Release patch does not match $FILC_SRC" >&2
  exit 1
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
  # lld links clang much faster than ld.bfd, but only use it when the host
  # compiler can actually link with it: an installed but broken ld.lld (e.g.
  # built against a libxml2 the system no longer has) makes the configure step
  # fail outright. LLVM_ENABLE_LLD=ON/OFF in the environment overrides this.
  if [ -z "${LLVM_ENABLE_LLD:-}" ]; then
    LLD_PROBE=$(mktemp -d)
    if printf 'int main(){return 0;}\n' > "$LLD_PROBE/probe.cpp" &&
       "${CXX:-c++}" -fuse-ld=lld -o "$LLD_PROBE/probe" "$LLD_PROBE/probe.cpp" \
         >/dev/null 2>&1; then
      LLVM_ENABLE_LLD=ON
    else
      LLVM_ENABLE_LLD=OFF
      echo "== the host compiler cannot link with lld; using its default linker"
    fi
    rm -rf "$LLD_PROBE"
  fi
  echo "== configuring"
  cmake -S "$FILC_SRC/llvm" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DLLVM_ENABLE_PROJECTS=clang \
    -DLLVM_ENABLE_LLD="$LLVM_ENABLE_LLD" \
    -DLLVM_TARGETS_TO_BUILD=X86 \
    -DLLVM_ENABLE_ASSERTIONS=OFF
fi

# opt drives the FilAsync pass tests (compiler/dev) and `make cfg`.
echo "== building clang and opt (this is the long part)"
ninja -C "$BUILD_DIR" -j "$JOBS" clang opt
ln -sfn clang "$BUILD_DIR/bin/filcc"

# The source-built driver looks for its Fil-C runtime at <bin>/../../pizfix.
# Point that at the distribution's pizfix so it finds crt1.o, the headers and
# yolort; without it the driver falls back to the host's glibc headers.
FILC_ROOT=${FILC_ROOT:-$REPO/vendor/filc-0.685-linux-x86_64}
PIZFIX_LINK=$(cd "$BUILD_DIR/.." && pwd)/pizfix
if [ ! -e "$PIZFIX_LINK" ]; then
  if [ -d "$FILC_ROOT/pizfix" ]; then
    echo "== linking $PIZFIX_LINK -> $FILC_ROOT/pizfix"
    ln -sfn "$FILC_ROOT/pizfix" "$PIZFIX_LINK"
  else
    echo "   WARNING: no pizfix at $FILC_ROOT; set FILC_ROOT to a Fil-C" >&2
    echo "            distribution before using $BUILD_DIR/bin/filcc" >&2
  fi
fi

echo "== done"
echo "   $BUILD_DIR/bin/clang"
echo
echo "Use it to link against a runtime built with the io_uring extension:"
echo "   $BUILD_DIR/bin/filcc -static -DFASYNC_COMPILER_INSERTS_CHECKS -I$REPO/runtime/src -L$REPO/runtime/build/lib ... -lpizlo -lfilc_async_uring -lpizlo -lc"
echo
echo "NOTE: this build itself needs the pizfix runtime from a Fil-C distribution."
echo "See wiki/Building-and-Linking.md for the full sequence."

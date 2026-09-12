#!/bin/sh
#
# build.sh -- build the async syscall runtime and splice it into libpizlo.a.
#
# WHAT GETS BUILT
# ---------------
# Three object files, compiled by two different compilers, spliced into a
# private copy of the distributed libpizlo.a:
#
#   pas-pizlo-release-filc_native_forwarders.o
#       REGENERATED from libpas's own generator after adding the io_uring
#       signatures. This replaces the distributed member of the same name. It is
#       what makes zsys_io_uring_* callable from memory-safe code.
#       Built by: host clang, against libpas's internal headers.
#
#   fil-pizlo-async-native.o   <-- fasync_native.c
#       The trusted implementations of the three io_uring syscalls.
#       Built by: host clang. This is the only place raw syscalls happen.
#
#   fil-pizlo-async.o          <-- fasync.c
#       The memory-safe runtime: rings, pending-request table, resolution,
#       provenance. Built by: filcc, so it is fully capability-checked.
#
# Note what is NOT rebuilt: the rest of libpizlo (libpas, filc_runtime, the GC).
# The extension is additive, so the distributed objects are reused as-is. The
# only member replaced is the forwarders table, because adding a bridged
# function necessarily changes it.
#
# Usage:
#   ./build.sh
#   FILC_ROOT=/path/to/filc-dist ./build.sh

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)

FILC_ROOT=${FILC_ROOT:-$REPO/vendor/filc-0.685-linux-x86_64}
FILC_SRC=${FILC_SRC:-$REPO/vendor/fil-c-src}
FILCC=${FILCC:-$FILC_ROOT/build/bin/filcc}
HOST_CLANG=${HOST_CLANG:-clang}

if [ ! -x "$FILCC" ]; then
  echo "build.sh: filcc not found at $FILCC" >&2
  echo "          set FILC_ROOT to a Fil-C distribution" >&2
  exit 1
fi
if [ ! -d "$FILC_SRC/libpas" ]; then
  echo "build.sh: Fil-C sources not found at $FILC_SRC" >&2
  echo "          set FILC_SRC, or run: git clone --depth 1 --filter=blob:none \\" >&2
  echo "            --sparse -b deluge https://github.com/pizlonator/fil-c \\" >&2
  echo "            vendor/fil-c-src && (cd vendor/fil-c-src && git sparse-checkout set filc libpas)" >&2
  exit 1
fi

GENERATOR=$FILC_SRC/libpas/src/libpas/generate_pizlonated_forwarders.rb
PATCH=$HERE/patches/0001-libpas-io_uring-forwarders.patch

BUILD=$HERE/build
OBJ=$BUILD/obj
LIB=$BUILD/lib
# Start clean: a stale object left in $OBJ would otherwise be re-spliced into the
# archive by a later step, which is a confusing way to ship an old binary.
rm -rf "$OBJ" "$LIB"
mkdir -p "$OBJ" "$LIB"

# ---------------------------------------------------------------------
# 0. Kernel headers for compiling the trusted half.
#
# libpas is normally built against an "os-include" tree of kernel headers
# assembled during a full Fil-C source build. Fil-C's build_os_include.sh builds
# it out of symlinks to the system's, and that is all it is, so we do the same
# rather than vendoring copies. Kept out of version control: absolute symlinks
# into /usr/include are not portable.
# ---------------------------------------------------------------------
if [ ! -e "$HERE/os-include/linux" ]; then
  echo "== creating os-include from the system kernel headers"
  mkdir -p "$HERE/os-include"
  ln -sfn /usr/include/linux "$HERE/os-include/linux"
  if [ -d "/usr/include/$(uname -m)-linux-gnu/asm" ]; then
    ln -sfn "/usr/include/$(uname -m)-linux-gnu/asm" "$HERE/os-include/asm"
  else
    ln -sfn /usr/include/asm "$HERE/os-include/asm"
  fi
  ln -sfn /usr/include/asm-generic "$HERE/os-include/asm-generic"
fi

# ---------------------------------------------------------------------
# 1. Make sure the io_uring signatures are present in the generator.
# ---------------------------------------------------------------------
if grep -q "zsys_io_uring_setup" "$GENERATOR"; then
  echo "== generator already carries the io_uring signatures"
else
  echo "== applying $PATCH"
  ( cd "$FILC_SRC" && git apply "$PATCH" )
fi

# ---------------------------------------------------------------------
# Include setup for compiling libpas-side code.
#
# libpas is normally compiled against a "yolo-include" tree of musl headers and
# an "os-include" tree of kernel headers, both built during a full Fil-C source
# build. The distribution does not ship them, but its pizfix/include tree is the
# same musl header set, so it stands in for yolo-include. Kernel headers come
# from runtime/os-include, which symlinks the system's.
# ---------------------------------------------------------------------
PAS_INCLUDES="-nostdinc -isystem $FILC_ROOT/pizfix/include \
  -isystem $HERE/os-include \
  -isystem $FILC_ROOT/pizfix/stdfil-include \
  -I $FILC_SRC/libpas/src/libpas \
  -DPAS_FILC=1"

# ---------------------------------------------------------------------
# 2. Regenerate the pizlonated forwarders.
# ---------------------------------------------------------------------
echo "== regenerating pizlonated forwarders (adds zsys_io_uring_*)"
( cd "$FILC_SRC/libpas" && \
  ruby src/libpas/generate_pizlonated_forwarders.rb src/libpas/filc_native.h && \
  ruby src/libpas/generate_pizlonated_forwarders.rb src/libpas/filc_native_forwarders.c )

echo "== compiling forwarders (host clang)"
# shellcheck disable=SC2086
"$HOST_CLANG" -O3 -fPIC -pthread $PAS_INCLUDES \
  -c -o "$OBJ/pas-pizlo-release-filc_native_forwarders.o" \
  "$FILC_SRC/libpas/src/libpas/filc_native_forwarders.c"

# ---------------------------------------------------------------------
# 3. Compile the native (trusted) half.
# ---------------------------------------------------------------------
echo "== compiling native io_uring implementations (host clang, unsafe)"
# shellcheck disable=SC2086
"$HOST_CLANG" -O3 -fPIC -pthread $PAS_INCLUDES \
  -c -o "$OBJ/fil-pizlo-async-native.o" "$HERE/src/fasync_native.c"

# ---------------------------------------------------------------------
# 4. Compile the memory-safe half.
# ---------------------------------------------------------------------
echo "== compiling async runtime (filcc, memory-safe, capability-checked)"
"$FILCC" -O3 -g -W -Werror -I"$HERE/src" \
  -c -o "$OBJ/fil-pizlo-async.o" "$HERE/src/fasync.c"
"$FILCC" -O3 -g -W -Werror -I"$HERE/src" \
  -c -o "$OBJ/fil-pizlo-dep.o" "$HERE/src/fasync_dep.c"

# ---------------------------------------------------------------------
# 5. Splice into a private copy of libpizlo.a.
#
# The member name of the forwarders object is deliberately identical to the
# distributed one, so `ar r` replaces it rather than adding a duplicate.
# ---------------------------------------------------------------------
echo "== splicing into a private copy of libpizlo.a"
cp "$FILC_ROOT/pizfix/lib/libpizlo.a" "$LIB/libpizlo.a"
( cd "$LIB" && ar r libpizlo.a \
    "$OBJ/pas-pizlo-release-filc_native_forwarders.o" \
    "$OBJ/fil-pizlo-async-native.o" \
    "$OBJ/fil-pizlo-async.o" \
    "$OBJ/fil-pizlo-dep.o" >/dev/null && ranlib libpizlo.a )

echo "== done"
echo "   $LIB/libpizlo.a"
echo
echo "Link programs against it with:"
echo "   filcc -static -I$HERE/src -L$LIB ..."

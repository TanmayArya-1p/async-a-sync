#!/bin/sh
# Builds the async framework and the runtimes into build/lib:
#   libpizlo.a            Fil-C's runtime plus framework/ and every native half
#   libfilc_async_uring.a runtime=io_uring (io_uring/)
#   libfilc_async_rpc.a   runtime=rpc (rpc/)
# Programs compile with -I runtime/include.
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

BUILD=$HERE/build
OBJ=$BUILD/obj
LIB=$BUILD/lib

rm -rf "$OBJ" "$LIB"
mkdir -p "$OBJ" "$LIB"


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

# patches/libpas-forwarders.patch adds the native entry points the runtimes
# call (the io_uring syscalls, the completion drain, the pending flag) to
# Fil-C's forwarder generator.
"$REPO/scripts/apply_filc_patches.sh" "$FILC_SRC" "$HERE/patches"

PAS_INCLUDES="-nostdinc -isystem $FILC_ROOT/pizfix/include \
  -isystem $HERE/os-include \
  -isystem $FILC_ROOT/pizfix/stdfil-include \
  -I $FILC_SRC/libpas/src/libpas \
  -DPAS_FILC=1"

echo "== regenerating pizlonated forwarders (adds zsys_io_uring_*)"
( cd "$FILC_SRC/libpas" && \
  ruby src/libpas/generate_pizlonated_forwarders.rb src/libpas/filc_native.h && \
  ruby src/libpas/generate_pizlonated_forwarders.rb src/libpas/filc_native_forwarders.c )

FILCC_FLAGS="-O3 -g -W -Werror -I$HERE/include"

echo "== compiling forwarders (host clang)"
# shellcheck disable=SC2086
"$HOST_CLANG" -O3 -fPIC -pthread $PAS_INCLUDES \
  -c -o "$OBJ/pas-pizlo-release-filc_native_forwarders.o" \
  "$FILC_SRC/libpas/src/libpas/filc_native_forwarders.c"

# framework/: the runtime-agnostic framework and its native half (the pending
# flag in a Fil-C object header).
echo "== compiling the framework"
# shellcheck disable=SC2086
"$HOST_CLANG" -O3 -fPIC -pthread $PAS_INCLUDES \
  -c -o "$OBJ/fil-pizlo-filc-async-native.o" "$HERE/framework/filc_async_native.c"
for src in arena:filc_async_arena async:filc_async; do
  # shellcheck disable=SC2086
  "$FILCC" $FILCC_FLAGS -c -o "$OBJ/fil-pizlo-${src%%:*}.o" \
    "$HERE/framework/${src#*:}.c"
done

# io_uring/: runtime=io_uring, its explicit fasync_* API, and its native half
# (the io_uring syscalls and the completion drain).
echo "== compiling the io_uring runtime"
# shellcheck disable=SC2086
"$HOST_CLANG" -O3 -fPIC -pthread $PAS_INCLUDES \
  -c -o "$OBJ/fil-pizlo-async-native.o" "$HERE/io_uring/fasync_native.c"
for src in fasync:fasync async-uring:filc_async_uring syscalls:fasync_syscalls \
           token:fasync_token dep:fasync_dep; do
  # shellcheck disable=SC2086
  "$FILCC" $FILCC_FLAGS -I"$HERE/io_uring" -c -o "$OBJ/fil-pizlo-${src%%:*}.o" \
    "$HERE/io_uring/${src#*:}.c"
done

# rpc/: runtime=rpc, one TCP request per call to the rpc demos' server.
echo "== compiling the rpc runtime"
# shellcheck disable=SC2086
"$FILCC" $FILCC_FLAGS -c -o "$OBJ/fil-rpc-runtime.o" "$HERE/rpc/rpc_runtime.c"

# The framework and every native half go into the private libpizlo.a: Fil-C's
# generated forwarders, which live there, call each native entry point, so
# the natives must be there too. Each runtime's memory-safe half is a library
# of its own, which a program links when its annotations name that runtime.
# A runtime calls the framework but not the other way round, so runtimes come
# first: -lfilc_async_rpc -lfilc_async_uring -lpizlo -lc.
echo "== splicing the framework into a private copy of libpizlo.a"
cp "$FILC_ROOT/pizfix/lib/libpizlo.a" "$LIB/libpizlo.a"
( cd "$LIB" && ar r libpizlo.a \
    "$OBJ/pas-pizlo-release-filc_native_forwarders.o" \
    "$OBJ/fil-pizlo-async-native.o" \
    "$OBJ/fil-pizlo-filc-async-native.o" \
    "$OBJ/fil-pizlo-arena.o" \
    "$OBJ/fil-pizlo-async.o" >/dev/null && ranlib libpizlo.a )
echo "== archiving libfilc_async_uring.a and libfilc_async_rpc.a"
rm -f "$LIB/libfilc_async_uring.a" "$LIB/libfilc_async_rpc.a"
( cd "$LIB" && ar rc libfilc_async_uring.a \
    "$OBJ/fil-pizlo-fasync.o" \
    "$OBJ/fil-pizlo-async-uring.o" \
    "$OBJ/fil-pizlo-syscalls.o" \
    "$OBJ/fil-pizlo-token.o" \
    "$OBJ/fil-pizlo-dep.o" && ranlib libfilc_async_uring.a )
( cd "$LIB" && ar rc libfilc_async_rpc.a "$OBJ/fil-rpc-runtime.o" &&
    ranlib libfilc_async_rpc.a )

echo "== done"
echo "   $LIB/libpizlo.a"
echo "   $LIB/libfilc_async_uring.a"
echo "   $LIB/libfilc_async_rpc.a"
echo
echo "   filcc -static -I$HERE/include -L$LIB ... -lfilc_async_uring -lpizlo -lc"

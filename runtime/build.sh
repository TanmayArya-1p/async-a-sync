#!/bin/sh
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
OVERRIDE=$HERE/upstream-overrides/generate_pizlonated_forwarders.rb

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

echo "== installing the forwarders-generator override"
cp "$OVERRIDE" "$GENERATOR"

PAS_INCLUDES="-nostdinc -isystem $FILC_ROOT/pizfix/include \
  -isystem $HERE/os-include \
  -isystem $FILC_ROOT/pizfix/stdfil-include \
  -I $FILC_SRC/libpas/src/libpas \
  -DPAS_FILC=1"

echo "== regenerating pizlonated forwarders (adds zsys_io_uring_*)"
( cd "$FILC_SRC/libpas" && \
  ruby src/libpas/generate_pizlonated_forwarders.rb src/libpas/filc_native.h && \
  ruby src/libpas/generate_pizlonated_forwarders.rb src/libpas/filc_native_forwarders.c )

echo "== compiling forwarders (host clang)"
# shellcheck disable=SC2086
"$HOST_CLANG" -O3 -fPIC -pthread $PAS_INCLUDES \
  -c -o "$OBJ/pas-pizlo-release-filc_native_forwarders.o" \
  "$FILC_SRC/libpas/src/libpas/filc_native_forwarders.c"
echo "== compiling native io_uring implementations (host clang, unsafe)"
# shellcheck disable=SC2086
"$HOST_CLANG" -O3 -fPIC -pthread $PAS_INCLUDES \
  -c -o "$OBJ/fil-pizlo-async-native.o" "$HERE/src/fasync_native.c"

echo "== compiling async runtime (filcc, memory-safe, capability-checked)"
"$FILCC" -O3 -g -W -Werror -I"$HERE/src" \
  -c -o "$OBJ/fil-pizlo-async.o" "$HERE/src/fasync.c"
"$FILCC" -O3 -g -W -Werror -I"$HERE/src" \
  -c -o "$OBJ/fil-pizlo-syscalls.o" "$HERE/src/fasync_syscalls.c"
"$FILCC" -O3 -g -W -Werror -I"$HERE/src" \
  -c -o "$OBJ/fil-pizlo-token.o" "$HERE/src/fasync_token.c"
"$FILCC" -O3 -g -W -Werror -I"$HERE/src" \
  -c -o "$OBJ/fil-pizlo-dep.o" "$HERE/src/fasync_dep.c"
echo "== splicing into a private copy of libpizlo.a"
cp "$FILC_ROOT/pizfix/lib/libpizlo.a" "$LIB/libpizlo.a"
( cd "$LIB" && ar r libpizlo.a \
    "$OBJ/pas-pizlo-release-filc_native_forwarders.o" \
    "$OBJ/fil-pizlo-async-native.o" \
    "$OBJ/fil-pizlo-async.o" \
    "$OBJ/fil-pizlo-syscalls.o" \
    "$OBJ/fil-pizlo-token.o" \
    "$OBJ/fil-pizlo-dep.o" >/dev/null && ranlib libpizlo.a )

echo "== done"
echo "   $LIB/libpizlo.a"
echo
echo "   filcc -static -I$HERE/src -L$LIB ..."

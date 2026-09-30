#!/bin/sh
# Verify that runtime/patches/libpas-forwarders.patch makes Fil-C's forwarder
# generator emit every native bridge the runtimes call. Needs Ruby and the
# Fil-C source checkout the patch applies to; it runs on a copy of the
# upstream generator, so the checkout is left alone.
set -eu

if ! command -v ruby >/dev/null 2>&1; then
  echo "check_forwarders: ruby not found" >&2
  exit 77
fi

REPO=$(cd "$(dirname "$0")/.." && pwd)
FILC_SRC=${FILC_SRC:-$REPO/vendor/fil-c-src}
GENERATOR=libpas/src/libpas/generate_pizlonated_forwarders.rb
if ! git -C "$FILC_SRC" cat-file -e "HEAD:$GENERATOR" 2>/dev/null; then
  echo "check_forwarders: no Fil-C source checkout at $FILC_SRC" >&2
  exit 77
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
mkdir -p "$TMP/libpas/src/libpas" "$TMP/out/src/libpas"
git -C "$FILC_SRC" show "HEAD:$GENERATOR" > "$TMP/$GENERATOR"
(cd "$TMP" && git apply "$REPO/runtime/patches/libpas-forwarders.patch")

(
  cd "$TMP/out"
  ruby "$TMP/$GENERATOR" src/libpas/filc_native.h
  ruby "$TMP/$GENERATOR" src/libpas/filc_native_forwarders.c
)

HEADER=$TMP/out/src/libpas/filc_native.h
FORWARDERS=$TMP/out/src/libpas/filc_native_forwarders.c
for name in zsys_io_uring_setup zsys_io_uring_enter \
            fasync_publish_state fasync_poll fasync_block \
            zasync_set_pending zasync_set_resolver; do
  grep -qF "filc_native_${name}(" "$HEADER" || {
    echo "missing native declaration: $name" >&2
    exit 1
  }
  grep -qF "filc_native_${name}(my_thread" "$FORWARDERS" || {
    echo "missing native call bridge: $name" >&2
    exit 1
  }
  grep -qF "filc_ptr pizlonated_${name}(" "$FORWARDERS" || {
    echo "missing linker forwarder: $name" >&2
    exit 1
  }
done
echo "CHECK_FORWARDERS PASS"

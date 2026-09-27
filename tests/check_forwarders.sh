#!/bin/sh
# Verify the checked-in generator emits every native io_uring bridge that the
# memory-safe runtime calls. This test needs Ruby, but not a Fil-C checkout.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
mkdir -p "$TMP/src/libpas"

(
  cd "$TMP"
  ruby "$REPO/runtime/upstream-overrides/generate_pizlonated_forwarders.rb" \
    src/libpas/filc_native.h
  ruby "$REPO/runtime/upstream-overrides/generate_pizlonated_forwarders.rb" \
    src/libpas/filc_native_forwarders.c
)

HEADER=$TMP/src/libpas/filc_native.h
FORWARDERS=$TMP/src/libpas/filc_native_forwarders.c
for name in zsys_io_uring_setup zsys_io_uring_enter zsys_io_uring_register \
            fasync_publish_state fasync_poll fasync_block; do
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

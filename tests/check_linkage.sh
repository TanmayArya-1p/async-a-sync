#!/bin/sh
# Check the archive members and the actual statically linked annotated binary.
# Usage: check_linkage.sh runtime/build/lib/libpizlo.a build/tests/t_linked_async
set -eu

ARCHIVE=${1:?pass the runtime libpizlo.a}
BINARY=${2:?pass the linked annotated executable}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

ar t "$ARCHIVE" > "$TMP/members"
nm -g --defined-only "$ARCHIVE" | awk 'NF >= 2 { print $NF }' > "$TMP/archive_symbols"
nm -g --defined-only "$BINARY" | awk 'NF >= 2 { print $NF }' > "$TMP/binary_symbols"
nm -u "$BINARY" | awk 'NF >= 1 { print $NF }' > "$TMP/undefined_symbols"

expect_exact() {
  if ! grep -qxF "$3" "$2"; then
    echo "missing $1: $3" >&2
    exit 1
  fi
}

expect_compiled() {
  if ! grep -Eq "(^|_)${2}$" "$1"; then
    echo "missing compiled symbol in $1: $2" >&2
    exit 1
  fi
}

for member in pas-pizlo-release-filc_native_forwarders.o \
              fil-pizlo-async-native.o fil-pizlo-fasync.o \
              fil-pizlo-async.o fil-pizlo-syscalls.o; do
  expect_exact 'archive member' "$TMP/members" "$member"
done

# Fil-C functions may carry a pizlonatedFIP prefix; bridges and forwarders do
# not.
for symbol in filc_async_submit filc_async_poll filc_async_wait \
              filc_async_mark_pending fasync_pread; do
  expect_compiled "$TMP/archive_symbols" "$symbol"
  expect_compiled "$TMP/binary_symbols" "$symbol"
done

for symbol in filc_resolve_pending \
              filc_native_zsys_io_uring_setup filc_native_zsys_io_uring_enter \
              filc_native_fasync_poll filc_native_fasync_block \
              pizlonated_zsys_io_uring_setup pizlonated_zsys_io_uring_enter \
              pizlonated_fasync_publish_state pizlonated_fasync_poll \
              pizlonated_fasync_block; do
  expect_exact 'archive symbol' "$TMP/archive_symbols" "$symbol"
  expect_exact 'linked binary symbol' "$TMP/binary_symbols" "$symbol"
done

expect_compiled "$TMP/binary_symbols" 'linked_pread'
for symbol in filc_async_submit filc_async_wait filc_resolve_pending \
              pizlonated_zsys_io_uring_setup pizlonated_zsys_io_uring_enter; do
  if grep -Eq "(^|_)${symbol}$" "$TMP/undefined_symbols"; then
    echo "unresolved implementation in executable: $symbol" >&2
    exit 1
  fi
done

echo "CHECK_LINKAGE PASS"
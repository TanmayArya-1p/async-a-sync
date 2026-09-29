#!/bin/sh
# Check the two archives and the actual statically linked annotated binary:
# the framework and the native bridges are in libpizlo.a, the io_uring runtime
# in libfilc_async_uring.a, and the framework archive does not define what a
# runtime must.
# Usage: check_linkage.sh runtime/build/lib build/tests/t_linked_async
set -eu

LIB=${1:?pass the runtime library directory}
BINARY=${2:?pass the linked annotated executable}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

ar t "$LIB/libpizlo.a" > "$TMP/framework_members"
ar t "$LIB/libfilc_async_uring.a" > "$TMP/runtime_members"
nm -g --defined-only "$LIB/libpizlo.a" | awk 'NF >= 2 { print $NF }' > "$TMP/framework_symbols"
nm -g --defined-only "$LIB/libfilc_async_uring.a" | awk 'NF >= 2 { print $NF }' > "$TMP/runtime_symbols"
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
              fil-pizlo-async-native.o fil-pizlo-filc-async-native.o \
              fil-pizlo-async.o fil-pizlo-arena.o; do
  expect_exact 'framework archive member' "$TMP/framework_members" "$member"
done
for member in fil-pizlo-fasync.o fil-pizlo-async-uring.o fil-pizlo-syscalls.o; do
  expect_exact 'runtime archive member' "$TMP/runtime_members" "$member"
done

# Fil-C compiled functions may carry a pizlonatedFIP signature prefix. The
# native bridges and generated forwarders have stable, unmangled names.
for symbol in filc_async_begin filc_async_poll filc_async_wait \
              filc_async_mark_pending filc_async_complete; do
  expect_compiled "$TMP/framework_symbols" "$symbol"
  expect_compiled "$TMP/binary_symbols" "$symbol"
done
for symbol in filc_async_submit filc_async_runtime_poll \
              filc_async_runtime_validate fasync_pread; do
  expect_compiled "$TMP/runtime_symbols" "$symbol"
  expect_compiled "$TMP/binary_symbols" "$symbol"
  # A Fil-C caller carries its own call thunk for a function it calls
  # (pizlonatedFI..._name); the definition itself is pizlonated_name.
  if grep -qxF "pizlonated_${symbol}" "$TMP/framework_symbols"; then
    echo "the framework archive defines the runtime's $symbol" >&2
    exit 1
  fi
done

for symbol in filc_resolve_pending \
              filc_native_zsys_io_uring_setup filc_native_zsys_io_uring_enter \
              filc_native_fasync_poll filc_native_fasync_block \
              pizlonated_zsys_io_uring_setup pizlonated_zsys_io_uring_enter \
              pizlonated_fasync_publish_state pizlonated_fasync_poll \
              pizlonated_fasync_block pizlonated_zasync_set_pending \
              pizlonated_zasync_set_resolver; do
  expect_exact 'framework archive symbol' "$TMP/framework_symbols" "$symbol"
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

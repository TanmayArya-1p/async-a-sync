#!/bin/sh
# Check the three archives and the actual statically linked annotated binary:
# the framework and the native bridges are in libpizlo.a, the io_uring runtime
# and its descriptor in libfilc_async_uring.a, the rpc runtime and its
# descriptor alone in libfilc_async_rpc.a, and the framework neither defines
# nor refers to anything of a runtime.
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
ar t "$LIB/libfilc_async_rpc.a" > "$TMP/rpc_members"
nm -g --defined-only "$LIB/libfilc_async_rpc.a" | awk 'NF >= 2 { print $NF }' > "$TMP/rpc_symbols"
nm -g --defined-only "$BINARY" | awk 'NF >= 2 { print $NF }' > "$TMP/binary_symbols"
nm -u "$BINARY" | awk 'NF >= 1 { print $NF }' > "$TMP/undefined_symbols"
(cd "$TMP" && ar x "$LIB/libpizlo.a" fil-pizlo-async.o)
nm -u "$TMP/fil-pizlo-async.o" | awk 'NF >= 1 { print $NF }' > "$TMP/framework_undefined"

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
              filc_async_mark_pending filc_async_complete filc_async_submit \
              filc_async_task_new; do
  expect_compiled "$TMP/framework_symbols" "$symbol"
  expect_compiled "$TMP/binary_symbols" "$symbol"
done
for symbol in filc_async_runtime_io_uring fasync_pread; do
  expect_compiled "$TMP/runtime_symbols" "$symbol"
  expect_compiled "$TMP/binary_symbols" "$symbol"
  # A Fil-C caller carries its own call thunk for a function it calls
  # (pizlonatedFI..._name); the definition itself is pizlonated_name.
  if grep -qxF "pizlonated_${symbol}" "$TMP/framework_symbols"; then
    echo "the framework archive defines the runtime's $symbol" >&2
    exit 1
  fi
done
# The rpc runtime is one object that defines its descriptor and nothing of
# the io_uring runtime's.
expect_exact 'rpc archive member' "$TMP/rpc_members" fil-rpc-runtime.o
expect_compiled "$TMP/rpc_symbols" filc_async_runtime_rpc
if grep -Eq "(fasync_|uring)" "$TMP/rpc_symbols"; then
  echo "the rpc archive defines io_uring runtime symbols" >&2
  exit 1
fi
if grep -Eq "(^|_)filc_async_runtime_rpc$" "$TMP/framework_symbols"; then
  echo "the framework archive defines the rpc runtime's descriptor" >&2
  exit 1
fi

# The framework reaches a runtime only through the descriptor a task's
# function names, so its object refers to no runtime at all.
if grep -Eq "(filc_async_runtime_|fasync_|uring)" "$TMP/framework_undefined"; then
  echo "the framework refers to a runtime:" >&2
  grep -E "(filc_async_runtime_|fasync_|uring)" "$TMP/framework_undefined" >&2
  exit 1
fi

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
for symbol in filc_async_submit filc_async_runtime_io_uring filc_async_wait \
              filc_resolve_pending \
              pizlonated_zsys_io_uring_setup pizlonated_zsys_io_uring_enter; do
  if grep -Eq "(^|_)${symbol}$" "$TMP/undefined_symbols"; then
    echo "unresolved implementation in executable: $symbol" >&2
    exit 1
  fi
done

echo "CHECK_LINKAGE PASS"

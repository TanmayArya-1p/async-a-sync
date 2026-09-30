#!/bin/sh
set -eu
REPO=$(cd "$(dirname "$0")/../.." && pwd)
if [ -z "${LLVM_CONFIG:-}" ]; then
    for candidate in llvm-config llvm-config-22 llvm-config-21 llvm-config-20 llvm-config-19; do
        if command -v "$candidate" >/dev/null 2>&1 &&
           [ -f "$("$candidate" --includedir)/llvm/Passes/PassPlugin.h" ]; then
            LLVM_CONFIG=$candidate
            break
        fi
    done
fi
if [ -z "${LLVM_CONFIG:-}" ]; then
    echo 'check_wait_all: LLVM development headers not found' >&2
    exit 77
fi
BINDIR=$("$LLVM_CONFIG" --bindir)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
PLUGIN=${FILASYNC_PLUGIN:-$TMP/plugin/libFilAsync.so}
if [ -z "${FILASYNC_PLUGIN:-}" ]; then
    cmake -S "$REPO/compiler/plugin" -B "$TMP/plugin" -G Ninja \
      -DLLVM_DIR="$("$LLVM_CONFIG" --cmakedir)" -DCMAKE_BUILD_TYPE=Release \
      >"$TMP/configure.log" 2>&1 || { cat "$TMP/configure.log"; exit 1; }
    cmake --build "$TMP/plugin" >"$TMP/build.log" 2>&1 || { cat "$TMP/build.log"; exit 1; }
fi
SRC=$REPO/tests/io_uring/t_wait_all_uring.c
"$BINDIR/clang" -O0 -DFASYNC_COMPILER_INSERTS_CHECKS -I"$REPO/runtime/include" \
  -Werror=pragma-clang-attribute -S -emit-llvm "$SRC" -o "$TMP/in.ll"
python3 "$REPO/tests/support/add_param_names.py" "$SRC" "$TMP/in.ll"
"$BINDIR/opt" -load-pass-plugin="$PLUGIN" -passes=filc-async \
  "$TMP/in.ll" -S -o "$TMP/out.ll"
"$BINDIR/clang" -c "$TMP/out.ll" -o "$TMP/out.o"
nm -u "$TMP/out.o" > "$TMP/undefined.txt"
objdump -dr "$TMP/out.o" > "$TMP/disassembly.txt"
python3 - "$TMP" <<'PY'
import pathlib
import re
import sys
p = pathlib.Path(sys.argv[1])
ir = (p / 'out.ll').read_text()
for name, marks in [('joined_read', 2), ('joined_write', 1)]:
    stub = re.search(r'define internal ptr @__filc_async_stub_' + name + r'\([^\n]+\) \{(.*?)^}', ir, re.S | re.M)
    assert stub, name
    body = stub.group(1)
    assert body.count('call void @filc_async_mark_pending') == marks, body
    assert 'call void @filc_async_mark_pending(ptr %task, ptr %4)' in body, body
    assert body.index('call void @filc_async_mark_pending') < body.index('call void @filc_async_submit'), body
    assert 'i64 80' in body and 'getelementptr i64, ptr %staging, i64 8' in body, body
    assert 'i64 5)' in body, body
    meta = next(line for line in ir.splitlines() if line.startswith('@__filc_meta_' + name + ' ='))
    assert '[5 x { i32, i32 }]' in meta and '{ i32, i32 } { i32 5, i32 0 }]' in meta, meta
undefined = (p / 'undefined.txt').read_text()
for symbol in ['filc_async_begin', 'filc_async_mark_pending', 'filc_async_submit',
               'filc_async_wait_all_array', 'filc_async_runtime_io_uring']:
    assert re.search(r'\bU ' + symbol + r'$', undefined, re.M), undefined
assert not re.search(r'\bU (joined_read|joined_write|fasync_\w+)$', undefined, re.M), undefined
assembly = (p / 'disassembly.txt').read_text()
assert len(re.findall(r'R_X86_64_\w+\s+filc_async_mark_pending-', assembly)) == 3, assembly
print('CHECK_WAIT_ALL_LOWERING PASS (5 argument cells, token marks before submission, generic relocations)')
PY

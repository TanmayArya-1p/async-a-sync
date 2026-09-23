#!/bin/sh
# opt_annotate_test.sh -- opt-level emission test for FilAsync (plan Task 3).
#
# Generates the two-function annotation fixture with HOST clang (Ruling-7: the
# patched clang's -S -emit-llvm output is already pizlonated, annotation
# pointers undef), runs the filc-async pass under the Fil-C LLVM-20 opt, and
# asserts the Task 3 brief's six greps plus the meta field-order/initializer
# check (Ruling-R5: struct field order + [nargs x {i32,i32}] tail, not the
# post-pizlonation 16-byte offsets) and the Ruling-3 ctor priority.
#
# Usage: ./compiler/dev/opt_annotate_test.sh
#        HOST_CC=/usr/bin/clang ./compiler/dev/opt_annotate_test.sh

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
SDD="$REPO/.superpowers/sdd/2026-09-23-pragma-async"
BUILD_DIR="$SDD/plugin-build"
PLUGIN="$BUILD_DIR/libFilAsync.so"
OPT="$REPO/vendor/fil-c-src/build/bin/opt"
LLVM_DIR="$REPO/vendor/fil-c-src/build/lib/cmake/llvm"
HOST_CC=${HOST_CC:-/usr/bin/clang}

TMP=/tmp/opencode
SRC="$TMP/two_annot.c"
IN="$TMP/two_annot.ll"
OUT="$TMP/out.ll"

mkdir -p "$TMP"

# Two annotated functions, each in its own push/pop, both called from main:
# declared-only-and-never-used functions get no llvm.global.annotations entry
# (Task 2 finding). Return types are pointers (Ruling-2 -> result = PTR = 2).
cat > "$SRC" <<'EOF'
#pragma clang attribute push(__attribute__((annotate("filc_async", "op=pread", "fd=0", "buf=1"))), apply_to=function)
void* procread(int fd, void* buf, unsigned long n);
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=openat", "fd=0", "buf=1"))), apply_to=function)
void* uopenat(int dirfd, const char* path, int flags, int mode);
#pragma clang attribute pop

int main(void) {
  void* a = procread(0, 0, 0);
  void* b = uopenat(0, 0, 0, 0);
  return a == b;
}
EOF

echo "### generating fixture IR with host clang"
"$HOST_CC" -S -emit-llvm -O0 -o "$IN" "$SRC"

echo "### building the plugin"
cmake -S "$REPO/compiler/plugin" -B "$BUILD_DIR" -G Ninja \
  -DLLVM_DIR="$LLVM_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD_DIR"

echo "### running filc-async"
"$OPT" -load-pass-plugin="$PLUGIN" -passes="filc-async" "$IN" -S -o "$OUT"

echo
FAIL=0
pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; FAIL=1; }

expect_grep() {
  if grep -qF -- "$1" "$OUT"; then
    pass "$2"
  else
    fail "$2 (missing: $1)"
  fi
}

# The brief's six greps.
expect_grep '@__filc_meta_procread' 'meta exists (@__filc_meta_procread)'
expect_grep '@__filc_opts_procread' 'opts exist (@__filc_opts_procread)'
expect_grep '@__filc_async_procread' 'renamed body (@__filc_async_procread)'
expect_grep '@__filc_async_meta_' 'per-TU meta table (@__filc_async_meta_*)'
expect_grep 'filc_async_ctor' 'table ctor (filc_async_ctor)'
expect_grep '@filc_async_validate_table' 'validator declared/called (@filc_async_validate_table)'

# Meta field-order / initializer check (Ruling-R5). Fixture facts:
#   procread: nargs=3, noped=2 (fd=,buf=), flags=0, result=PTR(2),
#             kinds: fd=0 -> 4 (ARG_FD), buf=1 -> 3 (ARG_BUFFER_OUT, op=pread),
#             arg2 -> 0 (ARG_IGNORED)
#   uopenat:  nargs=4, noped=2, flags=0, result=PTR(2),
#             kinds: 4 (ARG_FD), 2 (ARG_BUFFER_IN, op!=pread), 0, 0
# Struct field order must match the C header
# {name, nargs, noped_args, flags, result, opts, args[]}.
META_P=$(grep -F '@__filc_meta_procread =' "$OUT" || true)
if [ -n "$META_P" ]; then
  if echo "$META_P" | grep -qF -- '{ ptr, i32, i32, i32, i32, ptr, [3 x { i32, i32 }] }'; then
    pass 'procread meta struct field order + [nargs x {i32,i32}] tail'
  else
    fail 'procread meta struct field order + [nargs x {i32,i32}] tail'
  fi
  if echo "$META_P" | grep -qF -- 'i32 3, i32 2, i32 0, i32 2, ptr @__filc_opts_procread'; then
    pass 'procread meta values (nargs=3, noped=2, flags=0, result=PTR, opts)'
  else
    fail 'procread meta values (nargs=3, noped=2, flags=0, result=PTR, opts)'
  fi
  if echo "$META_P" | grep -qF -- '[{ i32, i32 } { i32 4, i32 0 }, { i32, i32 } { i32 3, i32 0 }, { i32, i32 } zeroinitializer]'; then
    pass 'procread args kinds (FD, BUFFER_OUT, IGNORED)'
  else
    fail 'procread args kinds (FD, BUFFER_OUT, IGNORED)'
  fi
else
  fail 'procread meta definition line not found (field-order check)'
fi

META_U=$(grep -F '@__filc_meta_uopenat =' "$OUT" || true)
if [ -n "$META_U" ]; then
  if echo "$META_U" | grep -qF -- '{ ptr, i32, i32, i32, i32, ptr, [4 x { i32, i32 }] }'; then
    pass 'uopenat meta struct field order + [4 x {i32,i32}] tail'
  else
    fail 'uopenat meta struct field order + [4 x {i32,i32}] tail'
  fi
  if echo "$META_U" | grep -qF -- 'i32 4, i32 2, i32 0, i32 2, ptr @__filc_opts_uopenat'; then
    pass 'uopenat meta values (nargs=4, noped=2, flags=0, result=PTR, opts)'
  else
    fail 'uopenat meta values (nargs=4, noped=2, flags=0, result=PTR, opts)'
  fi
  if echo "$META_U" | grep -qF -- '[{ i32, i32 } { i32 4, i32 0 }, { i32, i32 } { i32 2, i32 0 }, { i32, i32 } zeroinitializer, { i32, i32 } zeroinitializer]'; then
    pass 'uopenat args kinds (FD, BUFFER_IN, IGNORED, IGNORED)'
  else
    fail 'uopenat args kinds (FD, BUFFER_IN, IGNORED, IGNORED)'
  fi
else
  fail 'uopenat meta definition line not found (field-order check)'
fi

# Ruling-3: the table ctor must be LAST in init order (priority 65535, not 0).
expect_grep 'i32 65535, ptr @__filc_async_ctor' 'ctor registered at priority 65535 (Ruling-3)'
# Ruling-6: opts is [<nopts+1> x ptr], internal, trailing null.
expect_grep '@__filc_opts_procread = internal constant [4 x ptr]' 'opts array [nopts+1 x ptr], internal linkage'
expect_grep '@__filc_opts_procread = internal constant [4 x ptr] [ptr @' 'opts entries are real pointer constants'

echo
if [ "$FAIL" -eq 0 ]; then
  echo "=== OK: all emission checks passed"
else
  echo "=== FAILED: emission checks failed"
  exit 1
fi

#!/bin/sh
# opt_annotate_test.sh -- opt-level FIL async test (plan Task 3 emission +
# Task 4 call-site rewrite).
#
# Generates the two-function annotation fixture with HOST clang (Ruling-7: the
# patched clang's -S -emit-llvm output is already pizlonated, annotation
# pointers undef), runs the filc-async pass under the Fil-C LLVM-20 opt, and
# asserts:
#   - Task 3: the brief's six greps plus the meta field-order/initializer
#     check (Ruling-R5: struct field order + [nargs x {i32,i32}] tail, not the
#     post-pizlonation 16-byte offsets) and the Ruling-3 ctor priority;
#   - Task 4: the call-site rewrite greps (@filc_async_alloc / submit, the
#     call referencing its meta, original direct @procread/@uopenat calls gone,
#     staging stores), the annotation erasure (llvm.global.annotations and the
#     use-empty .args/.str globals gone while the opts-array .str globals stay
#     alive), and the Ruling-2 debug gate (-filc-async-debug prints "enrolled").
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
OUT_ERR="$TMP/out.err"

mkdir -p "$TMP"

# Two annotated functions, each in its own push/pop, both called from main:
# declared-only-and-never-used functions get no llvm.global.annotations entry
# (Task 2 finding). Return types are pointers (Ruling-2 -> result = PTR = 2).
# main passes VARIABLE arguments so the rewrite's zext/ptrtoint intval
# instructions are not folded away (literal 0s would become plain i64 0).
cat > "$SRC" <<'EOF'
#pragma clang attribute push(__attribute__((annotate("filc_async", "op=pread", "fd=0", "buf=1"))), apply_to=function)
void* procread(int fd, void* buf, unsigned long n);
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=openat", "fd=0", "buf=1"))), apply_to=function)
void* uopenat(int dirfd, const char* path, int flags, int mode);
#pragma clang attribute pop

int main(void) {
  int fd = 7;
  char c;
  void* buf = &c;
  unsigned long n = 1;
  void* a = procread(fd, buf, n);
  void* b = uopenat(fd, 0, 0, 0);
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

# Ruling-2: the "enrolled ..." lines print only under -filc-async-debug. opt
# dlopens the plugin mid-parse (the -load-pass-plugin callback fires while the
# command line is still being consumed), so this trailing flag is registered by
# the time it is looked up. Guard the invocation: against a plugin that does
# not register the flag (Task 3), opt dies on "Unknown command line argument"
# and the R2 assertion below reports FAIL.
echo "### running filc-async with -filc-async-debug (R2 gate check)"
if "$OPT" -load-pass-plugin="$PLUGIN" -filc-async-debug -passes="filc-async" \
    "$IN" -S -o /dev/null 2> "$OUT_ERR"; then
  DEBUG_OPT_OK=1
else
  DEBUG_OPT_OK=0
fi

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

# ---- Task 4: call-site rewrite (brief Step 1) ----
expect_grep '@filc_async_alloc' 'staging alloc present (@filc_async_alloc)'
expect_grep '@filc_async_submit' 'submit present (@filc_async_submit)'
expect_grep '@filc_async_submit(ptr @__filc_meta_procread' 'procread call references its meta'
expect_grep '@filc_async_submit(ptr @__filc_meta_uopenat' 'uopenat call references its meta'
expect_grep '@filc_async_submit(ptr @__filc_meta_procread, ptr @__filc_async_procread, ptr @__filc_opts_procread' \
  'submit passes meta, renamed impl, opts in order'

# Staging stores (Ruling-4): intval = zext(i32 fd/flags/mode) for integer
# params and ptrtoint(buffer) for the pointer param; capability word zeroed.
expect_grep 'zext i32 %' 'integer params zero-extended to intval (R4)'
expect_grep 'ptrtoint ptr %' 'buffer param ptrtoint-ed to intval (R4)'

# Original direct calls to the annotated functions are gone. Task 3's rename
# already renamed the declaration object itself, so a leftover direct call
# prints as `call ... @__filc_async_procread(...)` (not `@procread(...)`) --
# assert the load-bearing form: no `call` instruction may use the renamed body
# as its callee. The rewritten sites call @filc_async_submit instead. Keep the
# bare `@procread(`/`@uopenat(` sanity greps too.
if grep -qE 'call [^@]*@__filc_async_procread\(' "$OUT"; then
  fail 'direct call to @__filc_async_procread gone'
else
  pass 'direct call to @__filc_async_procread gone'
fi
if grep -qE 'call [^@]*@__filc_async_uopenat\(' "$OUT"; then
  fail 'direct call to @__filc_async_uopenat gone'
else
  pass 'direct call to @__filc_async_uopenat gone'
fi
if grep -qF -- '@procread(' "$OUT"; then
  fail 'bare @procread( absent (sanity)'
else
  pass 'bare @procread( absent (sanity)'
fi
if grep -qF -- '@uopenat(' "$OUT"; then
  fail 'bare @uopenat( absent (sanity)'
else
  pass 'bare @uopenat( absent (sanity)'
fi

# ---- Task 4: annotation erasure (R8) ----
if grep -qF 'llvm.global.annotations' "$OUT"; then
  fail 'llvm.global.annotations erased'
else
  pass 'llvm.global.annotations erased'
fi
if grep -qF '@.args' "$OUT"; then
  fail '.args globals erased (use-empty)'
else
  pass '.args globals erased (use-empty)'
fi
# The annotation-marker and source-file .str globals are referenced solely by
# llvm.global.annotations, so they die with it. The OPTION .str globals are
# kept alive by the opts arrays and must survive (below).
if grep -qF -- '@.str = ' "$OUT"; then
  fail 'annotation-name .str erased (only-use was llvm.global.annotations)'
else
  pass 'annotation-name .str erased (only-use was llvm.global.annotations)'
fi
if grep -qF -- '@.str.1 = ' "$OUT"; then
  fail 'annotation-file .str.1 erased (only-use was llvm.global.annotations)'
else
  pass 'annotation-file .str.1 erased (only-use was llvm.global.annotations)'
fi
expect_grep '@__filc_opts_procread = internal constant [4 x ptr] [ptr @.str' \
  'procread opts array still references its .str globals (retained)'
expect_grep '@__filc_opts_uopenat = internal constant [4 x ptr] [ptr @.str' \
  'uopenat opts array still references its .str globals (retained)'

# ---- Ruling-2: debug gate ----
if [ "$DEBUG_OPT_OK" -eq 1 ] && grep -qF 'enrolled procread' "$OUT_ERR"; then
  pass 'debug lines print under -filc-async-debug (R2 gate)'
else
  fail 'debug lines print under -filc-async-debug (R2 gate; flag rejected or lines missing)'
fi

echo
if [ "$FAIL" -eq 0 ]; then
  echo "=== OK: all checks passed"
else
  echo "=== FAILED: checks failed"
  exit 1
fi

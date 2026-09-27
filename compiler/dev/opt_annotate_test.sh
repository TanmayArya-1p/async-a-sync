#!/bin/sh
# opt_annotate_test.sh -- opt-level test of the FilAsync pass: descriptor
# emission, call-site rewriting, and annotation erasure.
#
# Generates the two-function annotation fixture with HOST clang (the patched
# clang's -S -emit-llvm output is already pizlonated, annotation pointers
# undef), runs the pass under the Fil-C LLVM-20 opt, and asserts:
#   - emission: the meta/opts/renamed-body/table/ctor greps, the meta
#     field-order/initializer check ({name,nargs,noped_args,flags,result,
#     opts,args[]} with a [nargs x {i32,i32}] tail), and the ctor's 65535
#     priority;
#   - rewrite: alloc/submit greps, each call referencing its meta, original
#     direct calls gone, staging scalar/pointer stores;
#   - erasure: llvm.global.annotations and use-empty .args/.str globals gone
#     while the opts-array .str globals stay alive;
#   - the -filc-async-debug gate (enrolled lines print only when enabled).
#
# Usage: ./compiler/dev/opt_annotate_test.sh
#        HOST_CC=/usr/bin/clang ./compiler/dev/opt_annotate_test.sh

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
BUILD_DIR=${BUILD_DIR:-$REPO/build/filasync-plugin}
PLUGIN="$BUILD_DIR/libFilAsync.so"
OPT="$REPO/vendor/fil-c-src/build/bin/opt"
LLVM_DIR="$REPO/vendor/fil-c-src/build/lib/cmake/llvm"
HOST_CC=${HOST_CC:-/usr/bin/clang}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
SRC="$TMP/two_annot.c"
IN="$TMP/two_annot.ll"
OUT="$TMP/out.ll"
OUT_ERR="$TMP/out.err"

if [ ! -x "$OPT" ]; then
  echo "$(basename "$0"): opt not found at $OPT" >&2
  echo "          build it with: ./compiler/build.sh" >&2
  exit 1
fi

# Two annotated functions, each declared and called from main: declared-but-
# unused functions get no llvm.global.annotations entry. Return types are
# pointers (-> result = PTR = 2). main passes VARIABLE arguments so the
# rewrite's scalar extension and pointer staging are not folded away. The
# annotations use the generic grammar (op never decides a kind): fd/bout on
# procread (FD + BUFFER_OUT), fd/bin + a bare buf (FD + BUFFER_IN + PENDING)
# on uopenat -- the bare buf= on an op=openat is what proves buf= is PENDING
# and NOT inferred from the op name.
cat > "$SRC" <<'EOF'
#pragma clang attribute push(__attribute__((annotate("filc_async", "op=pread", "fd=0", "bout=1"))), apply_to=function)
void* procread(int fd, void* buf, unsigned long n);
#pragma clang attribute pop

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=openat", "fd=0", "bin=1", "buf=2"))), apply_to=function)
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

# The "enrolled ..." lines print only under -filc-async-debug. opt dlopens
# the plugin mid-parse, so the flag is registered by the time a trailing
# -filc-async-debug token is looked up; a plugin that fails to register it
# makes opt die on "Unknown command line argument" and the check below FAILs.
echo "### running filc-async with -filc-async-debug (enrolled-lines gate)"
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

# Meta field-order / initializer check. Fixture facts:
#   procread: nargs=3, noped=2 (fd=,bout=), flags=0, result=PTR(2),
#             kinds: fd=0 -> 4 (ARG_FD), bout=1 -> 3 (ARG_BUFFER_OUT),
#             arg2 -> 0 (ARG_IGNORED)
#   uopenat:  nargs=4, noped=3 (fd=,bin=,buf=), flags=0, result=PTR(2),
#             kinds: 4 (ARG_FD), 2 (ARG_BUFFER_IN via bin=),
#             5 (ARG_PENDING via bare buf=, NOT inferred from op), 0
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
    pass 'procread args kinds (FD, BUFFER_OUT via bout=, IGNORED)'
  else
    fail 'procread args kinds (FD, BUFFER_OUT via bout=, IGNORED)'
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
  if echo "$META_U" | grep -qF -- 'i32 4, i32 3, i32 0, i32 2, ptr @__filc_opts_uopenat'; then
    pass 'uopenat meta values (nargs=4, noped=3, flags=0, result=PTR, opts)'
  else
    fail 'uopenat meta values (nargs=4, noped=3, flags=0, result=PTR, opts)'
  fi
  if echo "$META_U" | grep -qF -- '[{ i32, i32 } { i32 4, i32 0 }, { i32, i32 } { i32 2, i32 0 }, { i32, i32 } { i32 5, i32 0 }, { i32, i32 } zeroinitializer]'; then
    pass 'uopenat args kinds (FD, BUFFER_IN via bin=, PENDING via buf=, IGNORED)'
  else
    fail 'uopenat args kinds (FD, BUFFER_IN via bin=, PENDING via buf=, IGNORED)'
  fi
else
  fail 'uopenat meta definition line not found (field-order check)'
fi

# The table ctor must be LAST in init order (priority 65535, not 0).
expect_grep 'i32 65535, ptr @__filc_async_ctor' 'ctor registered at priority 65535 (Ruling-3)'
# opts is [<nopts+1> x ptr], internal, trailing null.
expect_grep '@__filc_opts_procread = internal constant [4 x ptr]' 'opts array [nopts+1 x ptr], internal linkage'
expect_grep '@__filc_opts_procread = internal constant [4 x ptr] [ptr @' 'opts entries are real pointer constants'

# ---- call-site rewriting ----
expect_grep '@filc_async_alloc' 'staging alloc present (@filc_async_alloc)'
expect_grep '@filc_async_submit' 'submit present (@filc_async_submit)'
expect_grep '@filc_async_submit(ptr @__filc_meta_procread' 'procread call references its meta'
expect_grep '@filc_async_submit(ptr @__filc_meta_uopenat' 'uopenat call references its meta'
expect_grep '@filc_async_submit(ptr @__filc_meta_procread, ptr @__filc_async_procread, ptr @__filc_opts_procread' \
  'submit passes meta, renamed impl, opts in order'

# Staging stores: scalar words are extended; pointer stores keep the Fil-C
# capability when FilPizlonator widens them.
expect_grep 'zext i32 %' 'integer params zero-extended to intval (R4)'
expect_grep 'store ptr %' 'pointer params staged with their capability'

# Original direct calls to the annotated functions are gone. The rename
# already renamed the declaration object itself, so a leftover direct call
# would print as `call ... @__filc_async_procread(...)` (not `@procread(...)`) --
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

# ---- annotation erasure ----
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
expect_grep '@__filc_opts_uopenat = internal constant [5 x ptr] [ptr @.str' \
  'uopenat opts array still references its .str globals (retained)'

# ---- debug gate: "enrolled ..." only under -filc-async-debug ----
if [ "$DEBUG_OPT_OK" -eq 1 ] && grep -qF 'enrolled procread' "$OUT_ERR"; then
  pass 'debug lines print under -filc-async-debug (R2 gate)'
else
  fail 'debug lines print under -filc-async-debug (R2 gate; flag rejected or lines missing)'
fi

# ---- mixed fixture: foreign annotations and an unprototyped call ----
# A non-filc_async annotation belongs to another consumer and must survive the
# erasure. kr() is called through an unprototyped declaration with fewer
# arguments than its definition takes; the pass must leave that call alone
# (with a diagnostic) rather than read operands the call does not have.
MIXED_SRC="$TMP/mixed.c"
MIXED_IN="$TMP/mixed.ll"
MIXED_OUT="$TMP/mixed_out.ll"
MIXED_ERR="$TMP/mixed.err"
cat > "$MIXED_SRC" <<'EOF'
void* kr();
int use_kr(void) { return kr(1) != 0; }

#pragma clang attribute push(__attribute__((annotate("filc_async", "op=ignore", "fd=0"))), apply_to=function)
void* kr(int a, int b) { return (void*)(long)(a + b); }
#pragma clang attribute pop

__attribute__((annotate("keep_me"))) int other(void) { return 0; }
int main(void) { return use_kr() + other(); }
EOF
"$HOST_CC" -std=gnu17 -Wno-deprecated-non-prototype -S -emit-llvm -O0 \
  -o "$MIXED_IN" "$MIXED_SRC"
if "$OPT" -load-pass-plugin="$PLUGIN" -passes="filc-async" "$MIXED_IN" -S \
    -o "$MIXED_OUT" 2> "$MIXED_ERR"; then
  pass 'mixed fixture: pass completes'
  ANNOT=$(grep -F '@llvm.global.annotations =' "$MIXED_OUT" || true)
  if echo "$ANNOT" | grep -qF '@other' && ! echo "$ANNOT" | grep -qF '@__filc_async_kr'; then
    pass 'foreign annotation kept, filc_async entry dropped'
  else
    fail 'foreign annotation kept, filc_async entry dropped'
  fi
  if grep -qF 'not a plain call matching its prototype' "$MIXED_ERR"; then
    pass 'mismatched-prototype call left in place with a diagnostic'
  else
    fail 'mismatched-prototype call left in place with a diagnostic'
  fi
else
  fail 'mixed fixture: pass completes'
  cat "$MIXED_ERR"
fi

echo
if [ "$FAIL" -eq 0 ]; then
  echo "=== OK: all checks passed"
else
  echo "=== FAILED: checks failed"
  exit 1
fi

#!/usr/bin/env bash
# make disasm: wordcount() from demo_wordcount.c compiled by GCC and by the
# patched Fil-C compiler, side by side, to show the pending-flag test the
# compiler puts before each access.
set -e

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$REPO/build/demos"
PATCHED_CC="${PATCHED_CC:-$REPO/vendor/fil-c-src/build/bin/filcc}"
mkdir -p "$BUILD_DIR"

gcc -O2 -o "$BUILD_DIR/wc_gcc" "$REPO/demos/wordcount/demo_wordcount.c"
"$REPO/runtime/build.sh" >/dev/null
"$PATCHED_CC" -O2 -static -DFASYNC_IMPLICIT -DFASYNC_COMPILER_INSERTS_CHECKS \
  -I "$REPO/runtime/include" -L "$REPO/runtime/build/lib" \
  -o "$BUILD_DIR/wc_filc_implicit" "$REPO/demos/wordcount/demo_wordcount.c" -lfilc_async_uring -lpizlo -lc

echo "Done building binaries."
echo

echo "================================================================="
echo " DISASSEMBLY COMPARISON: wordcount()"
echo "================================================================="
echo
echo "--- [GCC Disassembly: wordcount (Unchecked raw load)] ---"
objdump -d -M intel --no-show-raw-insn "$BUILD_DIR/wc_gcc" | awk '/<wordcount>:/,/<demo_now_ms>:/' | grep -v '<demo_now_ms>:'

echo
echo "--- [Fil-C Implicit Disassembly: wordcount (Compiler Hook Inserted)] ---"
WC_SYM=$(nm "$BUILD_DIR/wc_filc_implicit" | grep "wordcount" | awk '{print $3}' | head -n 1)
objdump -d -M intel --no-show-raw-insn "$BUILD_DIR/wc_filc_implicit" | sed -n "/<$WC_SYM>:/,/ret/p" | head -n 75

echo
echo "================================================================="
echo " Call Sites to filc_resolve_pending in wordcount():"
echo "================================================================="
objdump -d -M intel -C "$BUILD_DIR/wc_filc_implicit" | sed -n "/<$WC_SYM>:/,/ret/p" | grep -B 2 -A 3 "filc_resolve_pending"
echo "================================================================="

echo
echo "Notice in Fil-C implicit:"
echo "  -> 'call ... <filc_resolve_pending>' is emitted right before the capability-checked load!"
echo

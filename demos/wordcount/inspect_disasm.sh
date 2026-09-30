#!/usr/bin/env bash
# make disasm: count_words() from demo_wordcount.c compiled by GCC and by the
# patched Fil-C compiler, side by side, to show the pending-flag test the
# compiler puts before each access.
set -e

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="$REPO/build/demos"
PATCHED_CC="${PATCHED_CC:-$REPO/vendor/fil-c-src/build/bin/filcc}"
mkdir -p "$BUILD_DIR"

gcc -O2 -Wno-unused-result -o "$BUILD_DIR/wc_gcc" "$REPO/demos/wordcount/demo_wordcount.c"
"$REPO/runtime/build.sh" >/dev/null
"$PATCHED_CC" -O2 -static -DFASYNC_IMPLICIT -DFASYNC_COMPILER_INSERTS_CHECKS \
  -I "$REPO/runtime/include" -L "$REPO/runtime/build/lib" \
  -o "$BUILD_DIR/wc_filc_implicit" "$REPO/demos/wordcount/demo_wordcount.c" -lfilc_async_uring -lpizlo -lc

# count_words() is the loop that reads each buffer. Fil-C names it with a
# prefix, so find its symbol first.
FILC_SYM=$(nm "$BUILD_DIR/wc_filc_implicit" | awk '/count_words$/ {print $3; exit}')

echo "================================================================="
echo " DISASSEMBLY COMPARISON: count_words()"
echo "================================================================="
echo
echo "--- [GCC: count_words (plain load)] ---"
objdump -d -M intel --no-show-raw-insn --disassemble=count_words "$BUILD_DIR/wc_gcc" |
  sed -n '/<count_words>:/,$p'

echo
echo "--- [Fil-C implicit: $FILC_SYM (pending-flag test inserted)] ---"
objdump -d -M intel --no-show-raw-insn --disassemble="$FILC_SYM" "$BUILD_DIR/wc_filc_implicit" |
  sed -n "/<$FILC_SYM>:/,\$p" | head -n 75

echo
echo "================================================================="
echo " Calls to filc_resolve_pending in count_words():"
echo "================================================================="
objdump -d -M intel -C --disassemble="$FILC_SYM" "$BUILD_DIR/wc_filc_implicit" |
  grep -B 2 -A 3 "filc_resolve_pending"
echo "================================================================="

echo
echo "In the Fil-C build, 'call ... <filc_resolve_pending>' sits right before"
echo "the capability-checked load."
echo

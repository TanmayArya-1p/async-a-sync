#!/usr/bin/env bash
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO/build/demos"
OUT_DIR="$REPO/build/tests/wc"

mkdir -p "$BUILD_DIR" "$OUT_DIR"

FILC_ROOT="${FILC_ROOT:-$REPO/vendor/filc-0.685-linux-x86_64}"
FILCC="${FILCC:-$FILC_ROOT/build/bin/filcc}"
PATCHED_CC="${PATCHED_CC:-$REPO/vendor/fil-c-src/build/bin/filcc}"


gcc -O2 -I "$REPO/demos" -o "$BUILD_DIR/wc_gcc" "$REPO/demos/demo_wordcount.c"

if [ -x "$FILCC" ]; then
  "$FILCC" -O2 -static -I "$REPO/demos" -o "$BUILD_DIR/wc_filc_sync" "$REPO/demos/demo_wordcount.c"
fi

"$REPO/runtime/build.sh" >/dev/null
"$PATCHED_CC" -O2 -static -DFASYNC_IMPLICIT -DFASYNC_COMPILER_INSERTS_CHECKS \
  -I "$REPO/runtime/src" -I "$REPO/demos" -L "$REPO/runtime/build/lib" \
  -o "$BUILD_DIR/wc_filc_implicit" "$REPO/demos/demo_wordcount.c"

echo "Done building binaries."
echo

echo "================================================================="
echo " 1. DISASSEMBLY COMPARISON: wordcount()"
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

# Generate GCC tree CFG graph
(
  cd "$BUILD_DIR"
  gcc -O2 -I "$REPO/demos" -fdump-tree-cfg-graph "$REPO/demos/demo_wordcount.c" -o "$BUILD_DIR/wc_gcc_cfg_bin"
  DOT_FILE=$(find . -name "*demo_wordcount*.dot" | head -n 1)
  if [ -n "$DOT_FILE" ] && command -v dot >/dev/null 2>&1; then
    dot -Tpng "$DOT_FILE" -o "$BUILD_DIR/cfg_gcc_wordcount.png"
    echo "Generated GCC CFG image: $BUILD_DIR/cfg_gcc_wordcount.png"
  fi
)

# Generate Clang / Fil-C AST CFG Dump
echo "Generating Clang / Fil-C AST CFG dump..."
"$PATCHED_CC" -fsyntax-only -DFASYNC_IMPLICIT -DFASYNC_COMPILER_INSERTS_CHECKS \
  -I "$REPO/runtime/src" -I "$REPO/demos" \
  -Xclang -analyze -Xclang -analyzer-checker=debug.DumpCFG \
  "$REPO/demos/demo_wordcount.c" > "$BUILD_DIR/cfg_filc_wordcount.txt" 2>&1
echo "Generated Fil-C AST CFG dump: $BUILD_DIR/cfg_filc_wordcount.txt"

echo
echo "All analysis artifacts written to: $BUILD_DIR"

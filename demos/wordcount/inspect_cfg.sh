#!/bin/sh
# make cfg: the control-flow graph of wordcount() from demo_wordcount.c, from
# GCC and from the patched Fil-C compiler after instrumentation, where the
# pending-flag test adds a branch before each access. Writes .dot files, and
# .png images when Graphviz's dot is installed, to build/demos.
set -e

REPO=$(cd "$(dirname "$0")/../.." && pwd)
BUILD_DIR=$REPO/build/demos
PATCHED_CC=${PATCHED_CC:-$REPO/vendor/fil-c-src/build/bin/filcc}
OPT=${OPT:-$REPO/vendor/fil-c-src/build/bin/opt}
SRC=$REPO/demos/wordcount/demo_wordcount.c
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# png DOT PNG: renders DOT when dot is installed.
png() {
  if [ -f "$1" ] && command -v dot >/dev/null 2>&1; then
    dot -Tpng "$1" -o "$2"
    echo "   -> $2"
  fi
}

echo "1. GCC's tree CFG"
gcc -O2 -fdump-tree-cfg-graph "$SRC" -o wc_gcc_cfg_bin
png "$(find . -name '*demo_wordcount*.dot' | head -n 1)" cfg_gcc_wordcount.png

echo "2. Fil-C's CFG after instrumentation"
"$PATCHED_CC" -O2 -DFASYNC_IMPLICIT -DFASYNC_COMPILER_INSERTS_CHECKS \
  -I "$REPO/runtime/include" -emit-llvm -S "$SRC" -o wc_implicit.ll
"$OPT" -passes=dot-cfg -disable-output wc_implicit.ll >/dev/null 2>&1
png "$(ls .*_wordcount.dot 2>/dev/null | head -n 1)" cfg_filc_wordcount.png

echo "3. Clang's source-level CFG"
"$PATCHED_CC" -fsyntax-only -DFASYNC_IMPLICIT -DFASYNC_COMPILER_INSERTS_CHECKS \
  -I "$REPO/runtime/include" \
  -Xclang -analyze -Xclang -analyzer-checker=debug.DumpCFG \
  "$SRC" > cfg_filc_wordcount.txt 2>&1
echo "   -> $BUILD_DIR/cfg_filc_wordcount.txt"

echo "The CFGs are in $BUILD_DIR"

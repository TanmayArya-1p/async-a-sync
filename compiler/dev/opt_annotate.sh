#!/bin/sh
# opt_annotate.sh -- dev iteration loop for the FilAsync annotation reader.
#
# Rebuilds the loadable FilAsync pass plugin (Ruling-7 recipe: host clang for
# the fixture IR, the already-built Fil-C opt at vendor/fil-c-src/build/bin/opt
# to load it, plugin build dir under .superpowers/sdd/2026-09-23-pragma-async/
# so Tasks 3-4 can reuse it) and asserts the reader's contract on two fixtures:
#
#   good      -> opt prints "enrolled procread" + op=pread / fd=0 / buf=1, exit 0
#   malformed -> empty "op=" option; opt names procread and dies non-zero
#
# Usage: ./compiler/dev/opt_annotate.sh
#        HOST_CC=/usr/bin/clang ./compiler/dev/opt_annotate.sh

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
SDD="$REPO/.superpowers/sdd/2026-09-23-pragma-async"
BUILD_DIR="$SDD/plugin-build"
PLUGIN_SRC="$REPO/compiler/plugin"
PLUGIN="$BUILD_DIR/libFilAsync.so"
OPT="$REPO/vendor/fil-c-src/build/bin/opt"
LLVM_DIR="$REPO/vendor/fil-c-src/build/lib/cmake/llvm"
HOST_CC=${HOST_CC:-/usr/bin/clang}

TMP=/tmp/opencode
GOOD="$TMP/annot_only.ll"
BAD_SRC="$TMP/annot_malformed.c"
BAD="$TMP/annot_malformed.ll"

mkdir -p "$TMP"

echo "### generating fixtures"
"$HOST_CC" -S -emit-llvm -O0 -o "$GOOD" "$REPO/tests/t_annotate_smoke.c"
printf '%s\n' \
  '/* malformed: empty op= (Ruling-4 forbids it at compile time) */' \
  '#pragma clang attribute push(__attribute__((annotate("filc_async", "op=", "fd=0", "buf=1"))), apply_to=function)' \
  'int procread(int fd, void* buf, unsigned long n);' \
  '#pragma clang attribute pop' \
  'int main(void) { return procread(0, 0, 0); }' > "$BAD_SRC"
"$HOST_CC" -S -emit-llvm -O0 -o "$BAD" "$BAD_SRC"

echo "### building the plugin"
cmake -S "$PLUGIN_SRC" -B "$BUILD_DIR" -G Ninja \
  -DLLVM_DIR="$LLVM_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD_DIR"

echo
echo "### good fixture"
set +e
GOOD_OUT=$("$OPT" -load-pass-plugin="$PLUGIN" -passes="filc-async" "$GOOD" -disable-output 2>&1)
GOOD_RC=$?
set -e
echo "$GOOD_OUT"
[ "$GOOD_RC" -eq 0 ] || { echo "!! good fixture: opt exited $GOOD_RC"; exit 1; }
echo "$GOOD_OUT" | grep -q "enrolled procread" \
  || { echo "!! good fixture: missing 'enrolled procread'"; exit 1; }
for TOK in "op=pread" "fd=0" "buf=1"; do
  echo "$GOOD_OUT" | grep -q -- "$TOK" \
    || { echo "!! good fixture: missing parsed option '$TOK'"; exit 1; }
done

echo
echo "### malformed fixture (empty op=)"
set +e
BAD_OUT=$("$OPT" -load-pass-plugin="$PLUGIN" -passes="filc-async" "$BAD" -disable-output 2>&1)
BAD_RC=$?
set -e
echo "$BAD_OUT"
[ "$BAD_RC" -ne 0 ] \
  || { echo "!! malformed fixture: opt should have failed, exited $BAD_RC"; exit 1; }
echo "$BAD_OUT" | grep -q -- "procread" \
  || { echo "!! malformed fixture: diagnostic should name procread"; exit 1; }

echo
echo "=== OK"
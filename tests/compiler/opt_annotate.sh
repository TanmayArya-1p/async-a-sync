#!/bin/sh
# opt_annotate.sh -- dev loop for the FilAsync annotation reader.
#
# Rebuilds the loadable FilAsync pass plugin and asserts the reader's contract
# on two fixtures:
#
#   good   -> opt prints "enrolled procread" + op=pread / buf=buf, exit 0
#   unknown -> an unrecognized "op=" value is ACCEPTED: the op set is the
#              runtime's authority, not the compiler's, so opt must not reject it
#
# Uses HOST clang for the fixture IR: the patched clang's -S -emit-llvm output
# is pizlonated, so the pre-pizlonation annotation IR has to come from the host
# compiler. The plugin runs under the Fil-C LLVM opt.
#
# Usage: ./tests/compiler/opt_annotate.sh
#        HOST_CC=/usr/bin/clang ./tests/compiler/opt_annotate.sh

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
BUILD_DIR=${BUILD_DIR:-$REPO/build/filasync-plugin}
PLUGIN_SRC="$REPO/compiler/plugin"
PLUGIN="$BUILD_DIR/libFilAsync.so"
OPT="$REPO/vendor/fil-c-src/build/bin/opt"
LLVM_DIR="$REPO/vendor/fil-c-src/build/lib/cmake/llvm"
HOST_CC=${HOST_CC:-/usr/bin/clang}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
GOOD="$TMP/annot_only.ll"
BAD_SRC="$TMP/annot_malformed.c"
BAD="$TMP/annot_malformed.ll"

if [ ! -x "$OPT" ]; then
  echo "$(basename "$0"): opt not found at $OPT" >&2
  echo "          build it with: ./compiler/build.sh" >&2
  exit 1
fi

echo "### generating fixtures"
"$HOST_CC" -S -emit-llvm -O0 -o "$GOOD" "$HERE/t_annotate_smoke.c"
python3 "$REPO/tests/support/add_param_names.py" "$HERE/t_annotate_smoke.c" "$GOOD"
printf '%s\n' \
  '/* unknown op=; the runtime is the authority for the op set */' \
  '#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=somefutureop", "buf=buf"))), apply_to=function)' \
  'int procread(int fd, void* buf, unsigned long n);' \
  '#pragma clang attribute pop' \
  'int main(void) { return procread(0, 0, 0); }' > "$BAD_SRC"
"$HOST_CC" -S -emit-llvm -O0 -o "$BAD" "$BAD_SRC"
python3 "$REPO/tests/support/add_param_names.py" "$BAD_SRC" "$BAD"

echo "### building the plugin"
cmake -S "$PLUGIN_SRC" -B "$BUILD_DIR" -G Ninja \
  -DLLVM_DIR="$LLVM_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD_DIR"

echo
echo "### good fixture"
set +e
GOOD_OUT=$("$OPT" -load-pass-plugin="$PLUGIN" -filc-async-debug -passes="filc-async" "$GOOD" -disable-output 2>&1)
GOOD_RC=$?
set -e
echo "$GOOD_OUT"
[ "$GOOD_RC" -eq 0 ] || { echo "!! good fixture: opt exited $GOOD_RC"; exit 1; }
echo "$GOOD_OUT" | grep -q "enrolled procread" \
  || { echo "!! good fixture: missing 'enrolled procread'"; exit 1; }
for TOK in "op=pread" "buf=buf"; do
  echo "$GOOD_OUT" | grep -q -- "$TOK" \
    || { echo "!! good fixture: missing parsed option '$TOK'"; exit 1; }
done

echo
echo "### unknown-op fixture (op=somefutureop: pass must accept)"
set +e
BAD_OUT=$("$OPT" -load-pass-plugin="$PLUGIN" -filc-async-debug -passes="filc-async" "$BAD" -disable-output 2>&1)
BAD_RC=$?
set -e
echo "$BAD_OUT"
[ "$BAD_RC" -eq 0 ] \
  || { echo "!! unknown-op fixture: opt should have accepted it, exited $BAD_RC"; exit 1; }
echo "$BAD_OUT" | grep -q "enrolled procread" \
  || { echo "!! unknown-op fixture: missing 'enrolled procread'"; exit 1; }
echo "$BAD_OUT" | grep -q -- "op=somefutureop" \
  || { echo "!! unknown-op fixture: missing parsed option 'op=somefutureop'"; exit 1; }

echo
echo "=== OK"
#!/bin/sh
# A pragma region containing only a call must be a compiler error.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
if [ -z "${CLANG:-}" ] && [ -x "$REPO/vendor/fil-c-src/build/bin/clang" ]; then
    CLANG=$REPO/vendor/fil-c-src/build/bin/clang
fi
CLANG=${CLANG:-clang}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

cat > "$TMP/callsite.c" <<'EOF'
void *work(int key);
void *caller(int key) {
#pragma clang attribute push(__attribute__((annotate("filc_async", "op=fsync", "w_dep=0"))), apply_to=function)
    void *result = work(key);
#pragma clang attribute pop
    return result;
}
EOF

if "$CLANG" -fsyntax-only -Werror=pragma-clang-attribute \
    "$TMP/callsite.c" >"$TMP/stdout" 2>"$TMP/stderr"; then
    echo "FAIL: pragma on a call compiled" >&2
    exit 1
fi
if ! grep -q "unused attribute 'annotate'" "$TMP/stderr"; then
    cat "$TMP/stderr" >&2
    echo "FAIL: call was rejected for a different reason" >&2
    exit 1
fi

echo "CHECK_CALLSITE_PRAGMA PASS"
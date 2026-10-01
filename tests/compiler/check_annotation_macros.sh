#!/bin/sh
# FILC_ASYNC and the option macros must give a function the same annotation as
# the `#pragma clang attribute` form: the pass reads that annotation and
# nothing else, so equal strings mean equal behaviour.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
INCLUDE="$HERE/../../runtime/include"
CLANG=${CLANG:-clang}
if ! command -v "$CLANG" >/dev/null 2>&1; then
    echo "check_annotation_macros: $CLANG not found" >&2
    exit 77
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "check_annotation_macros: python3 not found" >&2
    exit 77
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

# A name that is also a macro must not be expanded inside an option.
cat > "$TMP/macros.c" <<'EOF2'
#include "filc_async_annotate.h"
#include <stddef.h>
#define close closed_by_macro
#define buf buffer_by_macro
FILC_ASYNC(io_uring, FILC_OP(pread), FILC_BOUT(buf), FILC_R_DEP(fd, file),
           FILC_W_DEP(buf, mem))
void* every(int fd, void* buf, size_t n, unsigned long off);
FILC_ASYNC(io_uring, FILC_OP(pwrite), FILC_BIN(buf), FILC_W_DEP(fd, file))
void* writes(int fd, const void* buf, size_t n, unsigned long off);
FILC_ASYNC(io_uring, FILC_OP(close))
void* closes(int fd);
FILC_ASYNC(io_uring, FILC_OP(fsync), FILC_R_DEP(fd, left), FILC_R_DEP(fd, right))
void* twice(int fd);
FILC_ASYNC(io_uring, FILC_OP(ignore), FILC_BUF(recv))
void* unknown(void* recv);
FILC_ASYNC(rpc, FILC_OP(step), FILC_OPTION(retries, 3))
void* custom(int port);
FILC_ASYNC(mock, FILC_OP(sum), FILC_BIN(in))
void* defined(const void* in) { return 0; }
EOF2

cat > "$TMP/pragmas.c" <<'EOF2'
#include <stddef.h>
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", "bout=buf", "r_dep=fd:file", "w_dep=buf:mem"))), apply_to=function)
void* every(int fd, void* buf, size_t n, unsigned long off);
#pragma clang attribute pop
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pwrite", "bin=buf", "w_dep=fd:file"))), apply_to=function)
void* writes(int fd, const void* buf, size_t n, unsigned long off);
#pragma clang attribute pop
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=close"))), apply_to=function)
void* closes(int fd);
#pragma clang attribute pop
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=fsync", "r_dep=fd:left", "r_dep=fd:right"))), apply_to=function)
void* twice(int fd);
#pragma clang attribute pop
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=ignore", "buf=recv"))), apply_to=function)
void* unknown(void* recv);
#pragma clang attribute pop
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=rpc", "op=step", "retries=3"))), apply_to=function)
void* custom(int port);
#pragma clang attribute pop
#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=mock", "op=sum", "bin=in"))), apply_to=function)
void* defined(const void* in) { return 0; }
#pragma clang attribute pop
EOF2

# Each function's annotation strings, as clang parsed them.
annotations() {
    "$CLANG" -fsyntax-only -Werror -Wall -Wextra -I"$INCLUDE" \
        -Xclang -ast-dump=json "$1" 2>"$TMP/stderr" | python3 -c '
import json, sys
def strings(node, out):
    if node.get("kind") == "StringLiteral":
        out.append(node["value"])
    for child in node.get("inner", []):
        strings(child, out)
def walk(node):
    if node.get("kind") == "FunctionDecl":
        for attr in node.get("inner", []):
            if attr.get("kind") == "AnnotateAttr":
                found = []
                strings(attr, found)
                print(node["name"], " ".join(found))
    for child in node.get("inner", []):
        walk(child)
walk(json.load(sys.stdin))
' | sort
}

annotations "$TMP/macros.c" > "$TMP/macros.out" || { cat "$TMP/stderr" >&2; exit 1; }
annotations "$TMP/pragmas.c" > "$TMP/pragmas.out" || { cat "$TMP/stderr" >&2; exit 1; }

if [ ! -s "$TMP/pragmas.out" ] || ! diff -u "$TMP/pragmas.out" "$TMP/macros.out"; then
    echo "FAIL: the macros annotate differently from the pragma form" >&2
    exit 1
fi

# A misspelt option macro is an error, not an option the compiler skips.
cat > "$TMP/typo.c" <<'EOF2'
#include "filc_async_annotate.h"
FILC_ASYNC(io_uring, FILC_OP(pread), FILC_BOUTT(buf))
void* typo(int fd, void* buf);
EOF2
if "$CLANG" -fsyntax-only -I"$INCLUDE" "$TMP/typo.c" >/dev/null 2>&1; then
    echo "FAIL: a misspelt option macro compiled" >&2
    exit 1
fi

echo "CHECK_ANNOTATION_MACROS PASS ($(wc -l < "$TMP/macros.out" | tr -d ' ') annotations match the pragma form)"

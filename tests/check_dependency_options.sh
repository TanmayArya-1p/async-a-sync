#!/bin/sh
# Check r_dep/w_dep placement and the emitted dependency metadata.
set -eu
ulimit -c 0

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
if [ -z "${LLVM_CONFIG:-}" ]; then
    for candidate in llvm-config llvm-config-22 llvm-config-21 \
                     llvm-config-20 llvm-config-19; do
        if command -v "$candidate" >/dev/null 2>&1 &&
           [ -f "$("$candidate" --includedir)/llvm/Passes/PassPlugin.h" ]; then
            LLVM_CONFIG=$candidate
            break
        fi
    done
fi
if [ -z "${LLVM_CONFIG:-}" ]; then
    echo "check_dependency_options: LLVM development headers not found" >&2
    exit 77
fi
LLVM_BINDIR=$("$LLVM_CONFIG" --bindir)
CLANG=${CLANG:-$LLVM_BINDIR/clang}
OPT=${OPT:-$LLVM_BINDIR/opt}
LLVM_DIR=${LLVM_DIR:-$("$LLVM_CONFIG" --cmakedir)}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

PLUGIN=${FILASYNC_PLUGIN:-$TMP/plugin/libFilAsync.so}
if [ -z "${FILASYNC_PLUGIN:-}" ]; then
    for tool in cmake ninja; do
        if ! command -v "$tool" >/dev/null 2>&1; then
            echo "check_dependency_options: $tool not found" >&2
            exit 77
        fi
    done
    cmake -S "$REPO/compiler/plugin" -B "$TMP/plugin" -G Ninja \
        -DLLVM_DIR="$LLVM_DIR" -DCMAKE_BUILD_TYPE=Release >"$TMP/cmake.log" 2>&1 || {
        cat "$TMP/cmake.log" >&2
        exit 1
    }
    cmake --build "$TMP/plugin" >"$TMP/build.log" 2>&1 || {
        cat "$TMP/build.log" >&2
        exit 1
    }
fi

SRC="$HERE/t_dependency_option_placement.c"
"$CLANG" -S -emit-llvm -O0 -Werror=pragma-clang-attribute \
    "$SRC" -o "$TMP/placement.ll"
"$OPT" -load-pass-plugin="$PLUGIN" -filc-async-debug -passes=filc-async \
    "$TMP/placement.ll" -S -o "$TMP/placement_out.ll" \
    2>"$TMP/placement_debug.err"

sed 's/r_dep=/read_dep=/g' "$SRC" > "$TMP/legacy_read.c"
sed 's/w_dep=/write_dep=/g' "$SRC" > "$TMP/legacy_write.c"
"$CLANG" -S -emit-llvm -O0 "$TMP/legacy_read.c" -o "$TMP/legacy_read.ll"
"$CLANG" -S -emit-llvm -O0 "$TMP/legacy_write.c" -o "$TMP/legacy_write.ll"

python3 - "$TMP/placement_out.ll" "$TMP/placement_debug.err" "$OPT" "$PLUGIN" \
    "$TMP/legacy_read.ll" "$TMP/legacy_write.ll" <<'PY'
import pathlib
import re
import subprocess
import sys

ir = pathlib.Path(sys.argv[1]).read_text()
debug = pathlib.Path(sys.argv[2]).read_text()
lines = ir.splitlines()
expected = {
    "declared": ("{ i32 4, i32 1 }", "{ i32 3, i32 6 }"),
    "merged": ("{ i32 4, i32 2 }",),
    "separate": ("{ i32 4, i32 1 }",),
    "overridden": ("{ i32 4, i32 2 }",),
}
for name, dependencies in expected.items():
    prefix = f"@__filc_meta_{name} ="
    meta = next((line for line in lines if line.startswith(prefix)), None)
    if meta is None or any(dep not in meta for dep in dependencies):
        raise SystemExit(f"FAIL: {name} has wrong dependency metadata")
    submit = f"@filc_async_submit(ptr @__filc_meta_{name}"
    if submit not in ir:
        raise SystemExit(f"FAIL: {name} call was not rewritten")

override = re.search(r"enrolled overridden\n((?:  [^\n]*\n)+)", debug)
if override is None or "  op=fsync\n" not in override.group(1) or \
   "  w_dep=0\n" not in override.group(1) or \
   "  op=close\n" in override.group(1) or \
   "  r_dep=0\n" in override.group(1):
    raise SystemExit("FAIL: definition did not override op and dependency options")

for source in sys.argv[5:]:
    result = subprocess.run(
        [sys.argv[3], f"-load-pass-plugin={sys.argv[4]}",
         "-passes=filc-async", source, "-disable-output"],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    if result.returncode == 0 or "use r_dep= or w_dep=" not in result.stderr:
        raise SystemExit(f"FAIL: obsolete option in {source} was not rejected")

print("CHECK_DEPENDENCY_OPTIONS PASS")
PY

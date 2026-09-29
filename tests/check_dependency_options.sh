#!/bin/sh
# Check r_dep/w_dep placement, the emitted dependency metadata, the :<name>
# namespace hash, and compile-time conflict rejection.
# Fixture IR comes from HOST clang: the vendored clang's -S -emit-llvm output is
# already pizlonated, but the pass reads pre-pizlonated annotations.
set -eu
ulimit -c 0

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)

HOST_CC=${HOST_CC:-/usr/bin/clang}
OPT=${OPT:-$REPO/vendor/fil-c-src/build/bin/opt}
LLVM_DIR=${LLVM_DIR:-$REPO/vendor/fil-c-src/build/lib/cmake/llvm}
if [ ! -x "$HOST_CC" ] || [ ! -x "$OPT" ] || [ ! -f "$LLVM_DIR/LLVMConfig.cmake" ]; then
    echo "check_dependency_options: need host clang and the vendored LLVM-20 opt" >&2
    exit 1
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

PLUGIN_DIR=${FILASYNC_PLUGIN_DIR:-$REPO/build/plugin}
PLUGIN=${FILASYNC_PLUGIN:-$PLUGIN_DIR/libFilAsync.so}
if [ -z "${FILASYNC_PLUGIN:-}" ] &&
   { [ ! -f "$PLUGIN" ] ||
     [ "$REPO/compiler/upstream-overrides/llvm/lib/Transforms/Instrumentation/FilAsync.cpp" -nt "$PLUGIN" ]; }; then
    cmake -S "$REPO/compiler/plugin" -B "$PLUGIN_DIR" -G Ninja \
        -DLLVM_DIR="$LLVM_DIR" -DCMAKE_BUILD_TYPE=Release >"$TMP/cmake.log" 2>&1 || {
        cat "$TMP/cmake.log" >&2
        exit 1
    }
    cmake --build "$PLUGIN_DIR" >"$TMP/build.log" 2>&1 || {
        cat "$TMP/build.log" >&2
        exit 1
    }
fi
[ -f "$PLUGIN" ] || {
    echo "check_dependency_options: plugin not found at $PLUGIN" >&2
    exit 1
}

SRC="$HERE/t_dependency_option_placement.c"
"$HOST_CC" -S -emit-llvm -O0 -Werror=pragma-clang-attribute \
    "$SRC" -o "$TMP/placement.ll"
"$OPT" -load-pass-plugin="$PLUGIN" -filc-async-debug -passes=filc-async \
    "$TMP/placement.ll" -S -o "$TMP/placement_out.ll" \
    2>"$TMP/placement_debug.err"

sed 's/r_dep=/read_dep=/g' "$SRC" > "$TMP/legacy_read.c"
sed 's/w_dep=/write_dep=/g' "$SRC" > "$TMP/legacy_write.c"
"$HOST_CC" -S -emit-llvm -O0 "$TMP/legacy_read.c" -o "$TMP/legacy_read.ll"
"$HOST_CC" -S -emit-llvm -O0 "$TMP/legacy_write.c" -o "$TMP/legacy_write.ll"

"$HOST_CC" -S -emit-llvm -O0 "$HERE/t_dep_conflict.c" -o "$TMP/conflict.ll"

python3 - "$TMP/placement_out.ll" "$TMP/placement_debug.err" "$OPT" "$PLUGIN" \
    "$TMP/legacy_read.ll" "$TMP/legacy_write.ll" "$TMP/conflict.ll" <<'PY'
import pathlib
import re
import subprocess
import sys

def ns_hash(name):
    h = 2166136261
    for b in name.encode("utf-8"):
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    h &= 0xFFFFFF
    return h if h else 1

# Bit 31 may be set, and LLVM prints such i32 constants as signed negatives.
def llvm_i32(v):
    return str(v - (1 << 32) if v >= (1 << 31) else v)

ir = pathlib.Path(sys.argv[1]).read_text()
debug = pathlib.Path(sys.argv[2]).read_text()
lines = ir.splitlines()
ns_a = ns_hash("slotA")
ns_b = ns_hash("slotB")
expected = {
    "declared": ("{ i32 4, i32 1 }", "{ i32 3, i32 6 }"),
    "merged": ("{ i32 4, i32 2 }",),
    "separate": ("{ i32 4, i32 1 }",),
    "overridden": ("{ i32 4, i32 2 }",),
    "named": (f"{{ i32 4, i32 {llvm_i32(1 | (ns_a << 8))} }}",
              f"{{ i32 3, i32 {llvm_i32(6 | (ns_b << 8))} }}"),
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

for source in sys.argv[5:7]:
    result = subprocess.run(
        [sys.argv[3], f"-load-pass-plugin={sys.argv[4]}",
         "-passes=filc-async", source, "-disable-output"],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    if result.returncode == 0 or "use r_dep= or w_dep=" not in result.stderr:
        raise SystemExit(f"FAIL: obsolete option in {source} was not rejected")

conflict = subprocess.run(
    [sys.argv[3], f"-load-pass-plugin={sys.argv[4]}",
     "-passes=filc-async", sys.argv[7], "-disable-output"],
    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
if conflict.returncode == 0 or \
   "conflicting dependencies on argument 0" not in conflict.stderr:
    raise SystemExit("FAIL: same-argument mode/namespace conflict was not rejected")

print("CHECK_DEPENDENCY_OPTIONS PASS")
PY
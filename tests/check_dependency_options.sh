#!/bin/sh
# Check r_dep/w_dep placement, the emitted dependency metadata including the
# :<name> namespace hash, the runtime each descriptor names, and the rejection
# of contradictory options, of a buffer option on an argument that is not a
# pointer, of a missing, malformed or doubled runtime=, and of the removed fd=.
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

"$CLANG" -S -emit-llvm -O0 "$HERE/t_dep_conflict.c" -o "$TMP/conflict.ll"
sed 's/:right/:/' "$HERE/t_dep_conflict.c" > "$TMP/empty_name.c"
"$CLANG" -S -emit-llvm -O0 "$TMP/empty_name.c" -o "$TMP/empty_name.ll"
printf '%s\n' \
  '#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", "bout=2"))), apply_to=function)' \
  'void* scalar_buffer(int fd, void* buf, unsigned long len, unsigned long offset);' \
  '#pragma clang attribute pop' \
  'void* invoke(int fd, void* buf) { return scalar_buffer(fd, buf, 1, 0); }' \
  > "$TMP/scalar_buffer.c"
"$CLANG" -S -emit-llvm -O0 "$TMP/scalar_buffer.c" -o "$TMP/scalar_buffer.ll"
# The same well-formed call with its runtime= option dropped, malformed, or
# given twice, and with the removed fd= option.
for variant in none:'' bad:'"runtime=1bad", ' two:'"runtime=io_uring", "runtime=other", ' \
               fd:'"runtime=io_uring", "fd=0", '; do
  name=${variant%%:*}
  printf '%s\n' \
    "#pragma clang attribute push(__attribute__((annotate(\"filc_async\", ${variant#*:}\"op=pread\", \"bout=1\"))), apply_to=function)" \
    'void* runtime_read(int fd, void* buf, unsigned long len, unsigned long offset);' \
    '#pragma clang attribute pop' \
    'void* invoke(int fd, void* buf) { return runtime_read(fd, buf, 1, 0); }' \
    > "$TMP/runtime_$name.c"
  "$CLANG" -S -emit-llvm -O0 "$TMP/runtime_$name.c" -o "$TMP/runtime_$name.ll"
done

python3 - "$TMP/placement_out.ll" "$TMP/placement_debug.err" "$OPT" "$PLUGIN" \
    "$TMP/legacy_read.ll" "$TMP/legacy_write.ll" "$TMP/conflict.ll" \
    "$TMP/empty_name.ll" "$TMP/scalar_buffer.ll" "$TMP/runtime_none.ll" \
    "$TMP/runtime_bad.ll" "$TMP/runtime_two.ll" "$TMP/runtime_fd.ll" <<'PY'
import pathlib
import re
import subprocess
import sys

# The pass's namespace hash: FNV-1a folded to 24 bits, 0 becoming 1.
def ns_hash(name):
    h = 2166136261
    for b in name.encode():
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    h &= 0xFFFFFF
    return h or 1

# LLVM prints an i32 with bit 31 set as a negative number.
def i32(v):
    return str(v - (1 << 32) if v >= 1 << 31 else v)

ir = pathlib.Path(sys.argv[1]).read_text()
debug = pathlib.Path(sys.argv[2]).read_text()
lines = ir.splitlines()
expected = {
    "declared": ("{ i32 0, i32 1 }", "{ i32 3, i32 6 }"),
    "merged": ("{ i32 0, i32 2 }",),
    "separate": ("{ i32 0, i32 1 }",),
    "overridden": ("{ i32 0, i32 2 }",),
    "named": (f"{{ i32 0, i32 {i32(1 | ns_hash('slotA') << 8)} }}",
              f"{{ i32 3, i32 {i32(6 | ns_hash('slotB') << 8)} }}"),
}
for name, dependencies in expected.items():
    prefix = f"@__filc_meta_{name} ="
    meta = next((line for line in lines if line.startswith(prefix)), None)
    if meta is None or any(dep not in meta for dep in dependencies):
        raise SystemExit(f"FAIL: {name} has wrong dependency metadata")
    if f"call ptr @__filc_async_stub_{name}(" not in ir:
        raise SystemExit(f"FAIL: {name} call was not redirected to its stub")
    submit = f"@__filc_meta_{name}, ptr @__filc_async_run_{name},"
    if submit not in ir:
        raise SystemExit(f"FAIL: {name} stub does not submit its meta and body")
    if "ptr @filc_async_runtime_io_uring," not in meta:
        raise SystemExit(f"FAIL: {name} descriptor does not name its runtime")
if "@filc_async_runtime_io_uring = external" not in ir:
    raise SystemExit("FAIL: the runtime descriptor is not an external reference")

override = re.search(r"enrolled overridden\n((?:  [^\n]*\n)+)", debug)
if override is None or "  op=fsync\n" not in override.group(1) or \
   "  w_dep=0\n" not in override.group(1) or \
   "  op=close\n" in override.group(1) or \
   "  r_dep=0\n" in override.group(1):
    raise SystemExit("FAIL: definition did not override op and dependency options")

def rejected(source, message):
    result = subprocess.run(
        [sys.argv[3], f"-load-pass-plugin={sys.argv[4]}",
         "-passes=filc-async", source, "-disable-output"],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    return result.returncode != 0 and message in result.stderr

for source in sys.argv[5:7]:
    if not rejected(source, "use r_dep= or w_dep="):
        raise SystemExit(f"FAIL: obsolete option in {source} was not rejected")
if not rejected(sys.argv[7], "conflicting dependencies on argument 0"):
    raise SystemExit("FAIL: two namespaces on one argument were not rejected")
if not rejected(sys.argv[8], "has an empty namespace name"):
    raise SystemExit("FAIL: an empty namespace name was not rejected")
if not rejected(sys.argv[9], "which is not a pointer"):
    raise SystemExit("FAIL: bout= on an integer argument was not rejected")
if not rejected(sys.argv[10], "names no runtime; add runtime=<name>"):
    raise SystemExit("FAIL: a function without runtime= was not rejected")
if not rejected(sys.argv[11], "does not name a runtime"):
    raise SystemExit("FAIL: a malformed runtime name was not rejected")
if not rejected(sys.argv[12], "names two runtimes, io_uring and other"):
    raise SystemExit("FAIL: two runtimes on one function were not rejected")
if not rejected(sys.argv[13], "is no longer an option"):
    raise SystemExit("FAIL: the removed fd= option was not rejected")

print("CHECK_DEPENDENCY_OPTIONS PASS")
PY

#!/bin/sh
# Check r_dep/w_dep placement, the emitted dependency metadata including the
# hash of <param>:<namespace>, the runtime each descriptor names, and the rejection
# of contradictory options, of a buffer option on an argument that is not a
# pointer, of a missing, malformed or doubled runtime=, of the removed fd=, and
# of an index, an unknown parameter name or a missing namespace; and the
# kinds pointer parameters take from const when no option names them.
set -eu
ulimit -c 0

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
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

# emit SOURCE OUT: IR as a stock clang writes it, plus the parameter names
# the patched clang would record.
emit() {
    "$CLANG" -S -emit-llvm -O0 -Werror=pragma-clang-attribute "$1" -o "$2"
    python3 "$REPO/tests/support/add_param_names.py" "$1" "$2"
}

SRC="$HERE/t_dependency_option_placement.c"
emit "$SRC" "$TMP/placement.ll"
"$OPT" -load-pass-plugin="$PLUGIN" -filc-async-debug -passes=filc-async \
    "$TMP/placement.ll" -S -o "$TMP/placement_out.ll" \
    2>"$TMP/placement_debug.err"

sed 's/r_dep=/read_dep=/g' "$SRC" > "$TMP/legacy_read.c"
sed 's/w_dep=/write_dep=/g' "$SRC" > "$TMP/legacy_write.c"
emit "$TMP/legacy_read.c" "$TMP/legacy_read.ll"
emit "$TMP/legacy_write.c" "$TMP/legacy_write.ll"

# Variants of one function with two dependency options: as written (they
# conflict), and with an empty namespace, an index instead of a name, a name
# no parameter has, and no namespace.
CONFLICT="$HERE/t_dep_conflict.c"
emit "$CONFLICT" "$TMP/conflict.ll"
for variant in empty_name:'s/:right/:/' index:'s/r_dep=fd:left/r_dep=0:left/' \
               unknown:'s/r_dep=fd:left/r_dep=nosuch:left/' \
               no_namespace:'s/r_dep=fd:right/r_dep=fd/'; do
  name=${variant%%:*}
  sed "${variant#*:}" "$CONFLICT" > "$TMP/$name.c"
  emit "$TMP/$name.c" "$TMP/$name.ll"
done
printf '%s\n' \
  '#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=pread", "bout=len"))), apply_to=function)' \
  'void* scalar_buffer(int fd, void* buf, unsigned long len, unsigned long offset);' \
  '#pragma clang attribute pop' \
  'void* invoke(int fd, void* buf) { return scalar_buffer(fd, buf, 1, 0); }' \
  > "$TMP/scalar_buffer.c"
emit "$TMP/scalar_buffer.c" "$TMP/scalar_buffer.ll"
# The same well-formed call with its runtime= option dropped, malformed, or
# given twice, and with the removed fd= option.
for variant in none:'' bad:'"runtime=1bad", ' two:'"runtime=io_uring", "runtime=other", ' \
               fd:'"runtime=io_uring", "fd=0", '; do
  name=${variant%%:*}
  printf '%s\n' \
    "#pragma clang attribute push(__attribute__((annotate(\"filc_async\", ${variant#*:}\"op=pread\", \"bout=buf\"))), apply_to=function)" \
    'void* runtime_read(int fd, void* buf, unsigned long len, unsigned long offset);' \
    '#pragma clang attribute pop' \
    'void* invoke(int fd, void* buf) { return runtime_read(fd, buf, 1, 0); }' \
    > "$TMP/runtime_$name.c"
  emit "$TMP/runtime_$name.c" "$TMP/runtime_$name.ll"
done

# A pointer with no buffer option takes its kind from its type: a pointer to
# const is an input, and an option overrides the type either way.
printf '%s\n' \
  '#pragma clang attribute push(__attribute__((annotate("filc_async", "runtime=io_uring", "op=ignore", "buf=forced", "bin=named"))), apply_to=function)' \
  'void* inferred(int fd, const void* in, char const* const* names, void* out, char* const fixed, const char** list, const char* forced, char* named);' \
  '#pragma clang attribute pop' \
  'void* invoke(char** p) { return inferred(0, p, (char const* const*)p, p, *p, (const char**)p, *p, *p); }' \
  > "$TMP/consts.c"
emit "$TMP/consts.c" "$TMP/consts.ll"
"$OPT" -load-pass-plugin="$PLUGIN" -passes=filc-async "$TMP/consts.ll" -S \
    -o "$TMP/consts_out.ll"
# The same IR without !filc_async.const, as a stock clang leaves it.
sed 's/ !filc_async.const ![0-9]*//' "$TMP/consts.ll" > "$TMP/no_consts.ll"
"$OPT" -load-pass-plugin="$PLUGIN" -passes=filc-async "$TMP/no_consts.ll" -S \
    -o "$TMP/no_consts_out.ll"

python3 - "$TMP/placement_out.ll" "$TMP/placement_debug.err" "$OPT" "$PLUGIN" \
    "$TMP/legacy_read.ll" "$TMP/legacy_write.ll" "$TMP/conflict.ll" \
    "$TMP/empty_name.ll" "$TMP/scalar_buffer.ll" "$TMP/runtime_none.ll" \
    "$TMP/runtime_bad.ll" "$TMP/runtime_two.ll" "$TMP/runtime_fd.ll" \
    "$TMP/index.ll" "$TMP/unknown.ll" "$TMP/no_namespace.ll" \
    "$TMP/consts_out.ll" "$TMP/no_consts_out.ll" <<'PY'
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
# The space hashes "<param>:<namespace>"; 1 read, 2 write, 4 pointer.
def dep(kind, bits, key):
    return f"{{ i32 {kind}, i32 {i32(bits | ns_hash(key) << 8)} }}"

expected = {
    "declared": (dep(0, 1, "fd:file"), dep(3, 6, "buf:mem")),
    "merged": (dep(0, 2, "fd:file"),),
    "separate": (dep(0, 1, "fd:file"),),
    "overridden": (dep(0, 2, "fd:file"),),
    "named": (dep(0, 1, "fd:slotA"), dep(3, 6, "buf:slotB")),
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
   "  w_dep=fd:file\n" not in override.group(1) or \
   "  op=close\n" in override.group(1) or \
   "  r_dep=fd:file\n" in override.group(1):
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
if not rejected(sys.argv[7], "conflicting dependencies on parameter fd"):
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
if not rejected(sys.argv[14], "names no parameter; use a parameter name"):
    raise SystemExit("FAIL: an argument index was not rejected")
if not rejected(sys.argv[15], "names no parameter; use a parameter name"):
    raise SystemExit("FAIL: an unknown parameter name was not rejected")
if not rejected(sys.argv[16], "needs a namespace: <param>:<namespace>"):
    raise SystemExit("FAIL: a dependency without a namespace was not rejected")

# Arg kinds: 0 ignored, 2 bin=, 5 buf= (and any unannotated pointer).
def kinds(path, name):
    meta = next((line for line in pathlib.Path(path).read_text().splitlines()
                 if line.startswith(f"@__filc_meta_{name} =")), "")
    # LLVM prints an all-zero pair as zeroinitializer.
    pairs = re.findall(r"\{ i32, i32 \} (zeroinitializer|\{ i32 (\d+),)", meta)
    return [int(k or 0) for _, k in pairs]

#            fd in names out fixed list forced named
expect = [0, 2, 2, 5, 5, 5, 5, 2]
if kinds(sys.argv[17], "inferred") != expect:
    raise SystemExit(f"FAIL: kinds from const are {kinds(sys.argv[17], 'inferred')},"
                     f" not {expect}")
if kinds(sys.argv[18], "inferred") != [0, 5, 5, 5, 5, 5, 5, 2]:
    raise SystemExit("FAIL: IR without !filc_async.const did not keep every"
                     " unannotated pointer pending")

print("CHECK_DEPENDENCY_OPTIONS PASS")
PY

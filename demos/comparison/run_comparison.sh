#!/bin/sh
# Builds both versions of one comparison, runs them on the same workload and
# prints them side by side.
#
# Usage: demos/comparison/run_comparison.sh io_read [OUT_DIR] [files] [bytes] [passes]
#   io_read  io_read_filc_async.c (our io_uring runtime, built by the patched
#            compiler) against io_read_liburing.c (liburing, built by CC)
# The baseline needs liburing's headers (liburing-dev); LIBURING_CFLAGS and
# LIBURING_LIBS point the build at another copy.
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
TASK=${1:-}
OUT=${2:-$REPO/build/tests}
shift $(($# < 2 ? $# : 2))
PATCHED_CC=${PATCHED_CC:-$REPO/vendor/fil-c-src/build/bin/filcc}

case $TASK in
    io_read) BASELINE=liburing ;;
    *) echo "usage: run_comparison.sh io_read [OUT_DIR] [files] [bytes] [passes]" >&2; exit 2 ;;
esac
if [ ! -x "$PATCHED_CC" ]; then
    echo "run_comparison.sh: patched compiler missing at $PATCHED_CC" >&2
    exit 1
fi
mkdir -p "$OUT"

# ours
"$PATCHED_CC" -O2 -static -DFASYNC_COMPILER_INSERTS_CHECKS \
    -I"$REPO/runtime/include" -L"$REPO/runtime/build/lib" \
    -o "$OUT/${TASK}_filc_async" "$HERE/${TASK}_filc_async.c" \
    -lfilc_async_uring -lpizlo -lc

# the baseline
LIBURING_LIBS=${LIBURING_LIBS:--luring}
# shellcheck disable=SC2086
if ! "${CC:-cc}" -O2 ${LIBURING_CFLAGS:-} -o "$OUT/${TASK}_$BASELINE" \
    "$HERE/${TASK}_$BASELINE.c" $LIBURING_LIBS; then
    echo "run_comparison.sh: could not build the $BASELINE baseline;" \
        "install liburing-dev (apt install liburing-dev)" >&2
    exit 1
fi

# Each program's exit status is in its RESULT line; tee would hide it.
"$OUT/${TASK}_filc_async" "$OUT" "$@" | tee "$OUT/${TASK}_filc_async.out" || :
"$OUT/${TASK}_$BASELINE" "$OUT" "$@" | tee "$OUT/${TASK}_$BASELINE.out" || :

# Lines of task code: between the markers, without blank or comment-only lines.
task_lines() {
    sed -n '/^\/\* task \*\//,/^\/\* end task \*\//p' "$1" |
        grep -v '^\s*$' | grep -v '^\s*/[/*]' | grep -cv '^\s*\*'
}

row() { # row <label> <program> <source>
    awk -v label="$1" -v lines="$(task_lines "$3")" '/^RESULT / {
        mbs = $3 * $4 / 1048576 / ($5 / 1000)
        printf "  %-22s %10.2f %13.2f %9.0f %12d  %s\n", label, $5, $6, mbs, lines, $7
    }' "$2.out"
}

echo
awk '/^RESULT / { printf "%s: %d cold files of %d bytes, median ms per pass\n", $2, $3, $4; exit }' \
    "$OUT/${TASK}_filc_async.out"
printf "  %-22s %10s %13s %9s %12s\n" version "total ms" "checksum ms" "MB/s" "task lines"
row "FILC_ASYNC (Fil-C)" "$OUT/${TASK}_filc_async" "$HERE/${TASK}_filc_async.c"
row "$BASELINE (${CC:-cc} -O2)" "$OUT/${TASK}_$BASELINE" "$HERE/${TASK}_$BASELINE.c"
echo "  checksum ms: the checksum alone, over bytes already in memory"

for version in filc_async "$BASELINE"; do
    if ! grep -q '^RESULT .* ok$' "$OUT/${TASK}_$version.out"; then
        echo "run_comparison.sh: ${TASK}_$version did not pass" >&2
        exit 1
    fi
done

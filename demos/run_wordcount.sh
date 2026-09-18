#!/bin/sh
#
# run_wordcount.sh -- one word-count program, two backends.
#
# The same demos/demo_wordcount.c is built and run twice:
#
#   sync:     plain Fil-C (falling back to cc if no Fil-C dist is present).
#             read_all_files() blocks; every byte costs a round trip.
#   implicit: patched Fil-C + the io_uring runtime. read_all_files() enqueues
#             and returns first; counting is what waits, and the first access
#             publishes the whole queue lazily -- no submit call exists.
#
# Prints the two measured timings and the ratio. The payload lives under
# $OUT/wc so it lands on a real filesystem: on tmpfs the reads are cached and
# the ratio is meaningless. A warm page cache makes a re-run pointless too.
#
# Usage:
#   ./demos/run_wordcount.sh [payload-dir]
#   OUT=/path ./demos/run_wordcount.sh
#   PATCHED_CC=/path/to/patched-filcc ./demos/run_wordcount.sh

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)

FILC_ROOT=${FILC_ROOT:-$REPO/vendor/filc-0.685-linux-x86_64}
FILCC=${FILCC:-$FILC_ROOT/build/bin/filcc}
PATCHED_CC=${PATCHED_CC:-$REPO/vendor/fil-c-src/build/bin/filcc}
OUT=${OUT:-$REPO/build/tests}
DIR=${1:-$OUT/wc}

if [ ! -x "$PATCHED_CC" ]; then
  echo "run_wordcount.sh: patched compiler not built at:" >&2
  echo "  $PATCHED_CC" >&2
  echo "build it with: ./compiler/build.sh" >&2
  exit 1
fi

"$REPO/runtime/build.sh"
mkdir -p "$OUT" "$DIR"

echo "== building the two backends of the same source"
"$PATCHED_CC" -O2 -static -DFASYNC_IMPLICIT -DFASYNC_COMPILER_INSERTS_CHECKS \
  -I"$REPO/runtime/src" -L"$REPO/runtime/build/lib" \
  -o "$OUT/wc_implicit" "$REPO/demos/demo_wordcount.c"

if [ -x "$FILCC" ]; then
  "$FILCC" -O2 -static -o "$OUT/wc_sync" "$REPO/demos/demo_wordcount.c"
else
  echo "  plain filcc not found ($FILCC); using cc for the sync arm"
  cc -O2 -o "$OUT/wc_sync" "$REPO/demos/demo_wordcount.c"
fi

sync_out=$("$OUT/wc_sync" "$DIR")
implicit_out=$("$OUT/wc_implicit" "$DIR")

echo
echo "== the same code, run two ways"
printf '%s\n%s\n' "$sync_out" "$implicit_out"

sync_ms=$(printf '%s\n' "$sync_out" | sed 's/^  sync *//' | awk '{print $1}')
implicit_ms=$(printf '%s\n' "$implicit_out" |
              sed 's/^  implicit *//' | awk '{print $1}')

awk -v s="$sync_ms" -v i="$implicit_ms" \
  'BEGIN { printf "  implicit finish in %.2fx the time\n", s / i }'
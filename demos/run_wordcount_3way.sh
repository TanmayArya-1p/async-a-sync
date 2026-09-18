#!/usr/bin/env bash
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
FILC_ROOT="${FILC_ROOT:-$REPO/vendor/filc-0.685-linux-x86_64}"
FILCC="${FILCC:-$FILC_ROOT/build/bin/filcc}"
PATCHED_CC="${PATCHED_CC:-$REPO/vendor/fil-c-src/build/bin/filcc}"
OUT="${OUT:-$REPO/build/tests}"
DIR="${1:-$OUT/wc}"

mkdir -p "$OUT" "$DIR"

if [ ! -x "$PATCHED_CC" ]; then
  echo "Error: Patched Fil-C compiler not found at $PATCHED_CC" >&2
  exit 1
fi

echo " word count over 512 Files (Cold Page Cache)"
echo "=================================================================="

"$REPO/runtime/build.sh" >/dev/null 2>&1

# 2. Build 3 versions from the exact same source file: demos/demo_wordcount.c
gcc -O2 -I "$REPO/demos" -o "$OUT/wc_gcc" "$REPO/demos/demo_wordcount.c"
"$PATCHED_CC" -O2 -static -DFASYNC_IMPLICIT -DFASYNC_COMPILER_INSERTS_CHECKS \
  -I "$REPO/runtime/src" -I "$REPO/demos" -L "$REPO/runtime/build/lib" \
  -o "$OUT/wc_implicit" "$REPO/demos/demo_wordcount.c"

HAVE_FILC_SYNC=0
if [ -x "$FILCC" ]; then
  "$FILCC" -O2 -static -I "$REPO/demos" -o "$OUT/wc_filc_sync" "$REPO/demos/demo_wordcount.c"
  HAVE_FILC_SYNC=1
fi


gcc_out=$("$OUT/wc_gcc" "$DIR")
implicit_out=$("$OUT/wc_implicit" "$DIR")

gcc_ms=$(printf '%s\n' "$gcc_out" | awk '{print $2}')
implicit_ms=$(printf '%s\n' "$implicit_out" | awk '{print $2}')

printf "  %-16s %7.2f ms  (%s)\n" "GCC (sync)" "$gcc_ms" "Standard C baseline"

if [ "$HAVE_FILC_SYNC" -eq 1 ]; then
  filc_sync_out=$("$OUT/wc_filc_sync" "$DIR")
  filc_sync_ms=$(printf '%s\n' "$filc_sync_out" | awk '{print $2}')
  printf "  %-16s %7.2f ms  (%s)\n" "Fil-C (sync)" "$filc_sync_ms" "Safe C baseline (bounds-checked)"
fi

printf "  %-16s %7.2f ms  (%s)\n" "Fil-C (implicit)" "$implicit_ms" "Transparent async io_uring"

echo "------------------------------------------------------------------"
echo "RESULTS & SPEEDUP RATIOS:"

if [ "$HAVE_FILC_SYNC" -eq 1 ]; then
  awk -v s="$filc_sync_ms" -v i="$implicit_ms" \
    'BEGIN { printf "  -> Implicit vs Fil-C Sync :  %.2fx speedup\n", s / i }'
fi

awk -v g="$gcc_ms" -v i="$implicit_ms" \
  'BEGIN { printf "  -> Implicit vs GCC Sync   :  %.2fx speedup\n", g / i }'

echo "=================================================================="

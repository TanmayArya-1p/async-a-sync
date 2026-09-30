#!/bin/sh
# Builds demo_wordcount.c three ways and times them over the same files:
#   C               the host compiler (CC), blocking reads
#   Fil-C blocking  the stock filcc, blocking reads
#   Fil-C implicit  the patched compiler with the io_uring runtime
# The first two are skipped when their compiler is missing. All three must
# count the same words.
#
# Usage: demos/wordcount/run_wordcount.sh [DIR]   (the files go in DIR)
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)

FILC_ROOT=${FILC_ROOT:-$REPO/vendor/filc-0.685-linux-x86_64}
FILCC=${FILCC:-$FILC_ROOT/build/bin/filcc}
PATCHED_CC=${PATCHED_CC:-$REPO/vendor/fil-c-src/build/bin/filcc}
CC=${CC:-cc}
OUT=${OUT:-$REPO/build/tests}
DIR=${1:-$OUT/wc}

if [ ! -x "$PATCHED_CC" ]; then
  echo "run_wordcount.sh: patched compiler not built at:" >&2
  echo "  $PATCHED_CC" >&2
  echo "build it with: ./compiler/build.sh" >&2
  exit 1
fi

"$REPO/runtime/build.sh" >/dev/null
mkdir -p "$OUT" "$DIR"

echo "== building the same source three ways"
ARMS=
if command -v "$CC" >/dev/null 2>&1; then
  "$CC" -O2 -o "$OUT/wc_c" "$HERE/demo_wordcount.c"
  ARMS="$ARMS c"
else
  echo "  $CC not found; no C arm"
fi
if [ -x "$FILCC" ]; then
  "$FILCC" -O2 -static -o "$OUT/wc_filc" "$HERE/demo_wordcount.c"
  ARMS="$ARMS filc"
else
  echo "  plain filcc not found ($FILCC); no Fil-C blocking arm"
fi
"$PATCHED_CC" -O2 -static -DFASYNC_IMPLICIT -DFASYNC_COMPILER_INSERTS_CHECKS \
  -I"$REPO/runtime/include" -L"$REPO/runtime/build/lib" \
  -o "$OUT/wc_implicit" "$HERE/demo_wordcount.c" -lfilc_async_uring -lpizlo -lc
ARMS="$ARMS implicit"

label() {
  case $1 in
    c) echo "C" ;;
    filc) echo "Fil-C blocking" ;;
    implicit) echo "Fil-C implicit" ;;
  esac
}

echo
echo "== the same code, run each way (page cache dropped before each run)"
words=
for arm in $ARMS; do
  # Each run prints "  <mode> <ms> ms  (<words> words)".
  out=$("$OUT/wc_$arm" "$DIR")
  ms=$(printf '%s\n' "$out" | awk '{print $2}')
  n=$(printf '%s\n' "$out" | sed -n 's/.*(\([0-9]*\) words).*/\1/p')
  printf "  %-15s %8.2f ms  (%s words)\n" "$(label "$arm")" "$ms" "$n"
  eval "ms_$arm=\$ms"
  if [ -n "$words" ] && [ "$n" != "$words" ]; then
    echo "run_wordcount.sh: $(label "$arm") counted $n words, not $words" >&2
    exit 1
  fi
  words=$n
done

echo
for arm in $ARMS; do
  [ "$arm" = implicit ] && continue
  eval "base=\$ms_$arm"
  awk -v b="$base" -v i="$ms_implicit" -v l="$(label "$arm")" \
    'BEGIN { printf "  %s time / Fil-C implicit time: %.2fx\n", l, b / i }'
done

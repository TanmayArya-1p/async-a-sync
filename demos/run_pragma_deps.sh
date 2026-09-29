#!/bin/sh
# run_pragma_deps.sh -- build and run the three pragma dependency demos.
#
# Usage: ./demos/run_pragma_deps.sh [workdir]
#
# Needs the patched compiler. The demos are annotated with the filc_async
# pragma, so built with the stock compiler they would link and run while
# silently doing nothing -- the annotated bodies would just execute inline.

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)

PATCHED_CC=${PATCHED_CC:-$REPO/vendor/fil-c-src/build/bin/filcc}
OUT=${OUT:-$REPO/build/demos}
DIR=${1:-$OUT}

if [ ! -x "$PATCHED_CC" ]; then
  echo "run_pragma_deps.sh: patched compiler not built at:" >&2
  echo "  $PATCHED_CC" >&2
  echo "build it with: ./compiler/build.sh" >&2
  exit 1
fi

"$REPO/runtime/build.sh"
mkdir -p "$OUT" "$DIR"

DEMOS="demo_pragma_nodeps demo_pragma_ptrdeps demo_pragma_mixdeps"

for name in $DEMOS; do
  "$PATCHED_CC" -O2 -static -Werror=pragma-clang-attribute \
    -DFASYNC_COMPILER_INSERTS_CHECKS \
    -I"$REPO/runtime/src" -L"$REPO/runtime/build/lib" \
    -o "$OUT/$name" "$HERE/$name.c" -lpizlo -lc
done

rc=0
for name in $DEMOS; do
  echo
  echo "== $name"
  if ! "$OUT/$name" "$DIR"; then
    echo "!!! $name reported a failure"
    rc=1
  fi
done

echo
if [ "$rc" -eq 0 ]; then
  echo "all three pragma demos OK"
else
  echo "pragma demos FAILED"
fi
exit "$rc"

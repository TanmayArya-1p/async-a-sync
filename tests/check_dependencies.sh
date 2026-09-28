#!/bin/sh
# Exercise the real generic scheduler with a deterministic fake io_uring layer.
set -eu
ulimit -c 0

REPO=$(cd "$(dirname "$0")/.." && pwd)
HOST_CC=${HOST_CC:-cc}
if ! command -v "$HOST_CC" >/dev/null 2>&1; then
  echo "check_dependencies: $HOST_CC not found" >&2
  exit 77
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

"$HOST_CC" -std=gnu11 -Wall -Wextra -Werror -pthread \
  -I"$REPO/tests/mock_include" -I"$REPO/runtime/src" \
  "$REPO/runtime/src/filc_async.c" "$REPO/tests/t_dependency_mock.c" \
  -o "$TMP/check_dependencies"
"$TMP/check_dependencies"

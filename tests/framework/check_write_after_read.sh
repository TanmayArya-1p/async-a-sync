#!/bin/sh
# Read marks: a call that writes a buffer waits for earlier calls reading it.
set -eu
ulimit -c 0

REPO=$(cd "$(dirname "$0")/../.." && pwd)
HOST_CC=${HOST_CC:-cc}
if ! command -v "$HOST_CC" >/dev/null 2>&1; then
  echo "check_write_after_read: $HOST_CC not found" >&2
  exit 77
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

"$HOST_CC" -std=gnu11 -Wall -Wextra -Werror -pthread \
  -I"$REPO/tests/support/mock_include" -I"$REPO/runtime/include" \
  "$REPO/runtime/framework/filc_async.c" "$REPO/tests/framework/t_write_after_read.c" \
  -o "$TMP/check_write_after_read"
"$TMP/check_write_after_read"

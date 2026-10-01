#!/bin/sh
set -eu
REPO=$(cd "$(dirname "$0")/../.." && pwd)
HOST_CC=${HOST_CC:-cc}
command -v "$HOST_CC" >/dev/null 2>&1 || exit 77
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
"$HOST_CC" -std=gnu11 -Wall -Wextra -Werror -pthread ${WAIT_ALL_CFLAGS:-} \
  -DHOST_MOCK -I"$REPO/tests/support/mock_include" -I"$REPO/runtime/include" \
  "$REPO/runtime/framework/filc_async.c" "$REPO/tests/framework/t_wait_all.c" \
  -o "$TMP/check_wait_all"
"$TMP/check_wait_all"

#!/bin/sh
# Builds one of the rpc demos with runtime=rpc and runs it against the
# server on a loopback port.
#
# Usage: demos/rpc/run_rpc_demo.sh counter|upload [OUT_DIR]
#   counter  demo_rpc_counter: runtime=rpc only, no io_uring linked
#   upload   demo_rpc_upload: runtime=io_uring and runtime=rpc together
# Needs the patched compiler (PATCHED_CC) and the libraries built by
# runtime/build.sh; the server is built with the host compiler (CC).
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
DEMO=${1:-}
OUT=${2:-$REPO/build/tests}
PATCHED_CC=${PATCHED_CC:-$REPO/vendor/fil-c-src/build/bin/filcc}

case $DEMO in
    counter) RUNTIMES= ;;
    upload) RUNTIMES=-lfilc_async_uring ;;
    *) echo "usage: run_rpc_demo.sh counter|upload [OUT_DIR]" >&2; exit 2 ;;
esac
if [ ! -x "$PATCHED_CC" ]; then
    echo "run_rpc_demo.sh: patched compiler missing at $PATCHED_CC" >&2
    exit 1
fi
mkdir -p "$OUT"
"${CC:-cc}" -O2 -Wall -o "$OUT/rpc_server" "$HERE/rpc_server.c"
# shellcheck disable=SC2086
"$PATCHED_CC" -O2 -static -Werror=pragma-clang-attribute \
    -DFASYNC_COMPILER_INSERTS_CHECKS \
    -I"$REPO/runtime/src" -L"$REPO/runtime/build/lib" \
    -o "$OUT/demo_rpc_$DEMO" \
    "$HERE/demo_rpc_$DEMO.c" "$HERE/rpc_runtime.c" $RUNTIMES -lpizlo -lc

"$OUT/rpc_server" > "$OUT/rpc_server.port" 2> "$OUT/rpc_server.err" &
server=$!
trap 'kill "$server" 2>/dev/null || :' EXIT
trap 'exit 1' HUP INT TERM

port=
tries=0
while [ -z "$port" ]; do
    port=$(sed -n 's/^PORT //p' "$OUT/rpc_server.port")
    if [ -n "$port" ]; then
        break
    fi
    if ! kill -0 "$server" 2>/dev/null || [ "$tries" -ge 100 ]; then
        echo "run_rpc_demo.sh: the server did not start" >&2
        cat "$OUT/rpc_server.err" >&2
        exit 1
    fi
    tries=$((tries + 1))
    sleep 0.05
done
"$OUT/demo_rpc_$DEMO" "$port" "$OUT"

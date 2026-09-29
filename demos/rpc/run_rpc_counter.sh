#!/bin/sh
# Builds demo_rpc_counter with its own runtime (runtime=rpc) and runs it
# against the counter server on a loopback port.
#
# Usage: demos/rpc/run_rpc_counter.sh [OUT_DIR]
# Needs the patched compiler (PATCHED_CC) and the framework library built by
# runtime/build.sh; the server is built with the host compiler (CC).
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
OUT=${1:-$REPO/build/tests}
PATCHED_CC=${PATCHED_CC:-$REPO/vendor/fil-c-src/build/bin/filcc}

if [ ! -x "$PATCHED_CC" ]; then
    echo "run_rpc_counter.sh: patched compiler missing at $PATCHED_CC" >&2
    exit 1
fi
mkdir -p "$OUT"
"${CC:-cc}" -O2 -Wall -o "$OUT/rpc_counter_server" "$HERE/rpc_counter_server.c"
# The program links no io_uring runtime: runtime=rpc is the only one it names.
"$PATCHED_CC" -O2 -static -Werror=pragma-clang-attribute \
    -DFASYNC_COMPILER_INSERTS_CHECKS \
    -I"$REPO/runtime/src" -L"$REPO/runtime/build/lib" \
    -o "$OUT/demo_rpc_counter" \
    "$HERE/demo_rpc_counter.c" "$HERE/rpc_runtime.c" -lpizlo -lc

"$OUT/rpc_counter_server" > "$OUT/rpc_counter.port" 2> "$OUT/rpc_counter.err" &
server=$!
trap 'kill "$server" 2>/dev/null || :' EXIT
trap 'exit 1' HUP INT TERM

port=
tries=0
while [ -z "$port" ]; do
    port=$(sed -n 's/^PORT //p' "$OUT/rpc_counter.port")
    if [ -n "$port" ]; then
        break
    fi
    if ! kill -0 "$server" 2>/dev/null || [ "$tries" -ge 100 ]; then
        echo "run_rpc_counter.sh: the server did not start" >&2
        cat "$OUT/rpc_counter.err" >&2
        exit 1
    fi
    tries=$((tries + 1))
    sleep 0.05
done
"$OUT/demo_rpc_counter" "$port"

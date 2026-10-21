# SPDX-License-Identifier: Apache-2.0
#
# corTestFunctions.sh - the verbs this repo's tests use; corTest sources it into every section
#
# A test starts a broker that loads THIS build of the plugin (by path - nothing is installed), on a port
# of its own, with ramDB as its store: no database server is needed. A test waits on and asserts on what
# the API says, never on the broker's log - the log is kept for reading when a test fails.
#
#   IOTALORA_BROKER   the broker binary (default: coraine on PATH - the coraine package, or
#                 `. <dir>/install/env` after coraine-build)
#   IOTALORA_PORT     its port (default 19029 - clear of a broker on 1026 and of coraine's own test ports)
#   IOTALORA_PLUGIN   the plugin under test (default: iotalora.so in this repo)
#   IOTALORA_TEST_DIR scratch directory: the bridge configuration, the devices' logs, the broker's pid and log
#
IOTALORA_BROKER="${IOTALORA_BROKER:-coraine}"
IOTALORA_PORT="${IOTALORA_PORT:-19029}"
IOTALORA_PLUGIN="${IOTALORA_PLUGIN:-${REPO_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}/iotalora.so}"
IOTALORA_TEST_DIR="${IOTALORA_TEST_DIR:-/tmp/iotalora-bridge-test}"


# -----------------------------------------------------------------------------
#
# brokerStart [broker options...] - start the broker with the plugin and $IOTALORA_TEST_DIR/bridges.json
#
# Returns once the port answers; 1 (with the log's tail on stderr) if the broker died on the way.
#
brokerStart() {
  brokerStop

  "$IOTALORA_BROKER" --port "$IOTALORA_PORT" --foreground --pretty-print 2 --httpEndpoint "http://localhost:$IOTALORA_PORT" \
                 --database ramDB --bridges "$IOTALORA_PLUGIN" --bridgeConfig "$IOTALORA_TEST_DIR/bridges.json" "$@" \
                 > "$IOTALORA_TEST_DIR/broker.log" 2>&1 &
  echo $! > "$IOTALORA_TEST_DIR/broker.pid"

  local i
  for ((i = 0; i < 100; i++)); do
    curl -s -o /dev/null "localhost:$IOTALORA_PORT/ngsi-ld/v1/types" && return 0
    if ! kill -0 "$(cat "$IOTALORA_TEST_DIR/broker.pid")" 2>/dev/null; then
      echo "brokerStart: the broker died - $IOTALORA_TEST_DIR/broker.log:" >&2
      tail -20 "$IOTALORA_TEST_DIR/broker.log" >&2
      return 1
    fi
    sleep 0.1
  done

  echo "brokerStart: no answer on port $IOTALORA_PORT after 10s" >&2
  return 1
}


# -----------------------------------------------------------------------------
#
# brokerStop - stop the broker brokerStart started (by its pid, never by name)
#
brokerStop() {
  local pidFile="$IOTALORA_TEST_DIR/broker.pid"
  [ -f "$pidFile" ] || return 0

  local pid i
  pid=$(cat "$pidFile")
  kill "$pid" 2>/dev/null
  for ((i = 0; i < 50; i++)); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.1
  done
  kill -9 "$pid" 2>/dev/null
  rm -f "$pidFile"
}

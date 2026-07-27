#!/bin/sh
set -eu

# Regression test for the fail-closed /v1/ dispatcher gate
# (src/samosa_gateway.c: v1_route_is_legacy_unauthenticated /
# require_ui_session in gateway_handler()).
#
# A prior branch commit ("Complete Phase 7", reverted) added
# unauthenticated, popen()-backed /v1/chutni/* routes reachable by any
# unauthenticated local page because no route in that family called
# require_ui_session() -- the dispatcher failed OPEN: an unrecognized /v1/
# path with no token fell through every check and only hit "not found" at
# the end, never an auth check. This test proves the opposite property
# directly: an unrecognized /v1/ path -- standing in for any future route a
# task adds without remembering to wire its own require_ui_session() call --
# is rejected with 401 before route matching, not with 404 after it. It also
# proves the closed legacy-exemption list still lets pre-existing
# unauthenticated routes (Chat) through unauthenticated, since retrofitting
# those is a separate, out-of-scope task (see the T1.2 evidence doc).

BUILD_DIR="${BUILD_DIR:-build}"
GATEWAY="${SAMOSA_COMPILED_GATEWAY:-./$BUILD_DIR/samosa-gateway}"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/v1_fail_closed.XXXXXX")
HOME_DIR="$TMP/home"
PORT=18986
PID=""

cleanup() {
  [ -z "$PID" ] || kill "$PID" 2>/dev/null || true
  [ -z "$PID" ] || wait "$PID" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

make samosa-gateway >/dev/null 2>&1 || true

mkdir -p "$HOME_DIR"
printf '<!doctype html><title>Compiled Samosa</title><meta name="samosa-ui-token" content="__SAMOSA_UI_TOKEN__">\n' >"$TMP/app.html"
printf 'png\n' >"$TMP/logo.png"

SAMOSA_HOME="$HOME_DIR" \
SAMOSA_PORT="$PORT" \
SAMOSA_BACKEND_PORT=$((PORT + 1)) \
SAMOSA_APP_HTML="$TMP/app.html" \
SAMOSA_APP_LOGO="$TMP/logo.png" \
  "$GATEWAY" >"$TMP/stdout.log" 2>"$TMP/stderr.log" &
PID=$!

i=0
while [ "$i" -lt 50 ]; do
  curl -fsS "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1 && break
  sleep 0.05; i=$((i + 1))
done
kill -0 "$PID" 2>/dev/null || { echo "FAIL: gateway did not start"; cat "$TMP/stderr.log" >&2; exit 1; }

TOKEN=$(cat "$HOME_DIR/run/ui-token")
[ -n "$TOKEN" ] || { echo "FAIL: no ui-token written"; exit 1; }

# 1. An unrecognized /v1/ path with NO token must fail closed (401), not
#    fail open to route-lookup and end at 404. This is the core property:
#    a brand new route nobody remembered to gate still cannot be reached.
STATUS=$(curl -sS -o "$TMP/r1.json" -w '%{http_code}' \
  "http://127.0.0.1:$PORT/v1/this-route-does-not-exist-yet")
[ "$STATUS" = "401" ] || { echo "FAIL: expected 401 for ungated unknown /v1/ path with no token, got $STATUS"; cat "$TMP/r1.json"; exit 1; }
grep -q '"code":"invalid_ui_token"' "$TMP/r1.json" || { echo "FAIL: expected invalid_ui_token code"; cat "$TMP/r1.json"; exit 1; }

# 2. The same unknown path WITH a valid token reaches route matching and
#    gets a real 404 (proving the gate checks the token, not the path).
STATUS=$(curl -sS -o "$TMP/r2.json" -w '%{http_code}' \
  -H "X-Samosa-Token: $TOKEN" \
  "http://127.0.0.1:$PORT/v1/this-route-does-not-exist-yet")
[ "$STATUS" = "404" ] || { echo "FAIL: expected 404 for unknown /v1/ path with a valid token, got $STATUS"; cat "$TMP/r2.json"; exit 1; }

# 3. A POST to an unrecognized /v1/ path with no token also fails closed
#    (the gate runs before method-specific route matching too).
STATUS=$(curl -sS -o "$TMP/r3.json" -w '%{http_code}' -X POST \
  "http://127.0.0.1:$PORT/v1/also-not-a-real-route" -d '{}')
[ "$STATUS" = "401" ] || { echo "FAIL: expected 401 for POST to unknown /v1/ path with no token, got $STATUS"; cat "$TMP/r3.json"; exit 1; }

# 4. Existing gated routes (T1.2/T1.3) still require the token -- the
#    blanket gate must not have swallowed their own behavior.
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/v1/setup/status")
[ "$STATUS" = "401" ] || { echo "FAIL: expected 401 for /v1/setup/status with no token, got $STATUS"; exit 1; }
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/setup/status")
[ "$STATUS" = "200" ] || { echo "FAIL: expected 200 for /v1/setup/status with a valid token, got $STATUS"; exit 1; }

# 5. The closed legacy exemption list still lets Chat through with no
#    token -- retrofitting it is explicitly out of scope for this gate.
STATUS=$(curl -sS -o "$TMP/r5.json" -w '%{http_code}' -X POST \
  "http://127.0.0.1:$PORT/v1/chat/completions" -d '{"messages":[{"role":"user","content":"hi"}]}')
[ "$STATUS" = "409" ] || { echo "FAIL: expected 409 model_required for unauthenticated legacy Chat route, got $STATUS"; cat "$TMP/r5.json"; exit 1; }

echo "test_v1_fail_closed_default.sh: PASS (unknown /v1/ routes fail closed by default; legacy exemptions and existing gated routes unaffected)"

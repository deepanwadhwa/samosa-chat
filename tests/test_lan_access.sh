#!/bin/sh
set -eu

BUILD_DIR="${BUILD_DIR:-build}"
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/samosa-lan-access.XXXXXX")
HOME_DIR="$TMP/home"
PORT=$((19480 + ($$ % 100)))
PID=

cleanup() {
  if [ -n "$PID" ]; then
    kill "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true
  fi
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

lan_address() {
  if [ "$(uname -s)" = Darwin ]; then
    ifconfig 2>/dev/null |
      awk '$1 == "inet" && $2 !~ /^127\./ && $2 !~ /^169\.254\./ { print $2; exit }'
  elif command -v hostname >/dev/null 2>&1; then
    hostname -I 2>/dev/null |
      awk '{ for (i=1; i<=NF; i++) if ($i ~ /^[0-9]+\./ && $i !~ /^127\./) { print $i; exit } }'
  fi
}

mkdir -p "$HOME_DIR/models/qwen"
printf 'fixture\n' >"$HOME_DIR/models/qwen/experts.bin"
printf 'fixture\n' >"$HOME_DIR/models/qwen/tokenizer_qwen36.json"
SAMOSA_HOME="$HOME_DIR" \
SAMOSA_PORT="$PORT" \
SAMOSA_BIND=0.0.0.0 \
SAMOSA_LAN=1 \
SAMOSA_LAN_PASSWORD=password1234 \
SAMOSA_QWEN_ENGINE="$ROOT/$BUILD_DIR/test_fake_openai_backend" \
SAMOSA_APP_HTML="$ROOT/assets/app.html" \
SAMOSA_APP_LOGO="$ROOT/assets/samosa-chat.png" \
SAMOSA_MODELS_CATALOG="$ROOT/assets/models.json" \
  "$ROOT/$BUILD_DIR/samosa-gateway" >"$TMP/stdout.log" 2>"$TMP/stderr.log" &
PID=$!

i=0
while [ "$i" -lt 100 ]; do
  curl --noproxy '*' -fsS --max-time 1 "http://127.0.0.1:$PORT/healthz" >"$TMP/health.json" 2>/dev/null && break
  sleep 0.05
  i=$((i + 1))
done
[ "$i" -lt 100 ] || { echo "FAIL: LAN gateway did not start" >&2; cat "$TMP/stderr.log" >&2; exit 1; }
grep -q '"listen_address":"0.0.0.0"' "$TMP/health.json"
grep -q '"lan_access":true' "$TMP/health.json"
grep -q '"lan_auth":"password"' "$TMP/health.json"
python3 -c 'import json,sys; json.load(open(sys.argv[1], encoding="utf-8"))' "$TMP/health.json"

TOKEN=$(tr -d '\r\n' <"$HOME_DIR/run/ui-token")
[ -n "$TOKEN" ]
LAN_IP=$(lan_address)
[ -n "$LAN_IP" ] || { echo "FAIL: no non-loopback IPv4 address found" >&2; exit 1; }
BASE="http://$LAN_IP:$PORT"

# Only the authenticated gateway is shared. The raw model backend on port + 1
# must stay loopback-only even though it inherits from a LAN-mode parent.
i=0
while [ "$i" -lt 100 ]; do
  curl --noproxy '*' -fsS --max-time 1 "http://127.0.0.1:$((PORT + 1))/healthz" >/dev/null 2>&1 && break
  sleep 0.05
  i=$((i + 1))
done
[ "$i" -lt 100 ] || { echo "FAIL: private model backend did not start" >&2; exit 1; }
if curl --noproxy '*' -fsS --max-time 1 "http://$LAN_IP:$((PORT + 1))/healthz" >/dev/null 2>&1; then
  echo "FAIL: private model backend is reachable from the LAN" >&2
  exit 1
fi

# Loopback remains convenient for the host Mac. A LAN peer sees only the login
# page and cannot call even a legacy route until the password creates a cookie.
LOCAL_STATUS=$(curl --noproxy '*' -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/")
[ "$LOCAL_STATUS" = 200 ]
STATUS=$(curl --noproxy '*' -sS --max-time 2 -o "$TMP/login.html" -w '%{http_code}' "$BASE/")
[ "$STATUS" = 200 ]
grep -q 'Samosa — Sign in' "$TMP/login.html"
if grep -q "$TOKEN" "$TMP/login.html"; then
  echo "FAIL: login page exposed the UI session token" >&2
  exit 1
fi

STATUS=$(curl --noproxy '*' -sS --max-time 2 -o "$TMP/chat-denied.json" -w '%{http_code}' \
  -X POST -H 'Content-Type: application/json' \
  -d '{"messages":[{"role":"user","content":"hello"}]}' \
  "$BASE/v1/chat/completions")
[ "$STATUS" = 401 ]

STATUS=$(curl --noproxy '*' -sS --max-time 2 -o "$TMP/wrong-password.json" -w '%{http_code}' \
  -X POST -H 'Content-Type: application/json' -d '{"password":"wrong"}' \
  "$BASE/v1/lan/login")
[ "$STATUS" = 401 ]
grep -q '"code":"invalid_lan_password"' "$TMP/wrong-password.json"

STATUS=$(curl --noproxy '*' -sS --max-time 2 -c "$TMP/cookies.txt" \
  -o "$TMP/login.json" -w '%{http_code}' -X POST -H 'Content-Type: application/json' \
  -d '{"password":"password1234"}' "$BASE/v1/lan/login")
[ "$STATUS" = 200 ]
grep -q 'samosa_lan' "$TMP/cookies.txt"

STATUS=$(curl --noproxy '*' -sS --max-time 2 -b "$TMP/cookies.txt" \
  -o "$TMP/root.html" -w '%{http_code}' "$BASE/")
[ "$STATUS" = 200 ]
grep -q "$TOKEN" "$TMP/root.html"

STATUS=$(curl --noproxy '*' -sS --max-time 2 -b "$TMP/cookies.txt" \
  -o "$TMP/remote-health.json" -w '%{http_code}' "$BASE/healthz")
[ "$STATUS" = 200 ]

# Browser requests from the shared address pass when both the private token
# and the exact same Origin/Host pair are present. A foreign origin still
# fails even when it somehow has the token.
ORIGIN="$BASE"
STATUS=$(curl --noproxy '*' -sS --max-time 2 -o "$TMP/setup.json" -w '%{http_code}' \
  -H "X-Samosa-Token: $TOKEN" -H "Origin: $ORIGIN" "$BASE/v1/setup/status")
[ "$STATUS" = 200 ]

STATUS=$(curl --noproxy '*' -sS --max-time 2 -o "$TMP/wrong-origin.json" -w '%{http_code}' \
  -H "X-Samosa-Token: $TOKEN" -H 'Origin: http://evil.example' "$BASE/v1/setup/status")
[ "$STATUS" = 403 ]
grep -q '"code":"origin_denied"' "$TMP/wrong-origin.json"

# pagehide uses sendBeacon, which cannot attach a custom header. Its body-token
# authentication must continue to work for a remote browser tab.
STATUS=$(curl --noproxy '*' -sS --max-time 2 -o "$TMP/lifecycle.json" -w '%{http_code}' \
  -X POST -H 'Content-Type: application/json' -H "Origin: $ORIGIN" \
  -d "{\"action\":\"close\",\"client_id\":\"lan-page\",\"token\":\"$TOKEN\",\"hidden\":false}" \
  "$BASE/v1/app/lifecycle")
[ "$STATUS" = 200 ]

curl --noproxy '*' -fsS --max-time 2 -X POST -H "X-Samosa-Token: $TOKEN" \
  "http://127.0.0.1:$PORT/v1/shutdown" >/dev/null
wait "$PID"
PID=

echo "test_lan_access.sh: PASS ($LAN_IP)"

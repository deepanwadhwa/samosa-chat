#!/bin/sh
set -eu

fail() {
  echo "$(basename "$0"): FAIL: $1" >&2
  exit 1
}

# T1.2 (docs/TASKS_UI_CHUTNI.md): profile and setup state, plus the §5.0
# UI-token/Origin session contract every new v1 route in this program uses.

BUILD_DIR="${BUILD_DIR:-build}"
GATEWAY="${SAMOSA_COMPILED_GATEWAY:-./$BUILD_DIR/samosa-gateway}"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/profile_setup_test.XXXXXX")
HOME_DIR="$TMP/home"
PORT=18985
PID=""

cleanup() {
  [ -z "$PID" ] || kill "$PID" 2>/dev/null || true
  [ -z "$PID" ] || wait "$PID" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM



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

[ -f "$HOME_DIR/run/ui-token" ] || { echo "FAIL: ui-token file was not created"; exit 1; }
TOKEN=$(cat "$HOME_DIR/run/ui-token")
[ "${#TOKEN}" = 64 ] || { echo "FAIL: token is not 64 hex chars (got ${#TOKEN})"; exit 1; }
case "$TOKEN" in *[!0-9a-f]*) echo "FAIL: token is not lowercase hex: $TOKEN"; exit 1;; esac
PERMS=$(stat -f '%Lp' "$HOME_DIR/run/ui-token" 2>/dev/null || stat -c '%a' "$HOME_DIR/run/ui-token")
[ "$PERMS" = "600" ] || { echo "FAIL: ui-token mode is $PERMS, expected 600"; exit 1; }

ORIGIN="http://127.0.0.1:$PORT"

# --- root HTML: token substitution + Cache-Control: no-store ---
ROOT_HEADERS=$(curl -fsS -D - -o "$TMP/root.html" "http://127.0.0.1:$PORT/")
echo "$ROOT_HEADERS" | grep -qi '^Cache-Control: no-store' || { echo "FAIL: root HTML missing Cache-Control: no-store"; echo "$ROOT_HEADERS"; exit 1; }
grep -q "$TOKEN" "$TMP/root.html" || { echo "FAIL: root HTML does not contain the substituted token"; cat "$TMP/root.html"; exit 1; }
if grep -q '__SAMOSA_UI_TOKEN__' "$TMP/root.html"; then
  echo "FAIL: root HTML still contains the raw placeholder"; exit 1
fi

# --- auth: missing token, wrong token, wrong origin, correct everything ---
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/v1/profile")
[ "$STATUS" = "401" ] || { echo "FAIL: expected 401 with no token, got $STATUS"; exit 1; }

STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -H "X-Samosa-Token: wrong" "http://127.0.0.1:$PORT/v1/profile")
[ "$STATUS" = "401" ] || { echo "FAIL: expected 401 with wrong token, got $STATUS"; exit 1; }

STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" -H "Origin: http://evil.example" "http://127.0.0.1:$PORT/v1/profile")
[ "$STATUS" = "403" ] || { echo "FAIL: expected 403 with wrong origin, got $STATUS"; exit 1; }

# --- no profile yet: 404, not a crash ---
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" -H "Origin: $ORIGIN" "http://127.0.0.1:$PORT/v1/profile")
[ "$STATUS" = "404" ] || { echo "FAIL: expected 404 before any profile exists, got $STATUS"; exit 1; }

# --- setup status starts at "name" ---
STATUS_JSON=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H "Origin: $ORIGIN" "http://127.0.0.1:$PORT/v1/setup/status")
printf '%s' "$STATUS_JSON" | grep -q '"next_step":"name"' || { echo "FAIL: expected next_step name"; echo "$STATUS_JSON"; exit 1; }

# --- name validation: empty/whitespace-only rejected ---
STATUS=$(curl -sS -o "$TMP/resp.json" -w '%{http_code}' -X PUT -H "X-Samosa-Token: $TOKEN" -H "Origin: $ORIGIN" \
  -H 'Content-Type: application/json' -d '{"name":"   "}' "http://127.0.0.1:$PORT/v1/profile")
[ "$STATUS" = "400" ] || { echo "FAIL: whitespace-only name should be 400, got $STATUS"; cat "$TMP/resp.json"; exit 1; }

# --- name validation: too long (81 repeated scalars) rejected ---
LONG_NAME=$(python3 -c "print('a' * 81)")
STATUS=$(curl -sS -o "$TMP/resp.json" -w '%{http_code}' -X PUT -H "X-Samosa-Token: $TOKEN" -H "Origin: $ORIGIN" \
  -H 'Content-Type: application/json' --data-binary "{\"name\":\"$LONG_NAME\"}" "http://127.0.0.1:$PORT/v1/profile")
[ "$STATUS" = "400" ] || { echo "FAIL: 81-char name should be 400, got $STATUS"; cat "$TMP/resp.json"; exit 1; }

# --- name validation: invalid UTF-8 rejected ---
python3 -c "
import json
body = b'{\"name\":\"' + b'\377\376' + b'\"}'
open('$TMP/invalid_utf8.json', 'wb').write(body)
"
STATUS=$(curl -sS -o "$TMP/resp.json" -w '%{http_code}' -X PUT -H "X-Samosa-Token: $TOKEN" -H "Origin: $ORIGIN" \
  -H 'Content-Type: application/json' --data-binary @"$TMP/invalid_utf8.json" "http://127.0.0.1:$PORT/v1/profile")
[ "$STATUS" = "400" ] || { echo "FAIL: invalid UTF-8 name should be 400, got $STATUS (note: malformed JSON itself may also 400)"; cat "$TMP/resp.json"; exit 1; }

# --- valid name: trimmed, escapes markup literally, name screen -> welcome ---
PUT_RESP=$(curl -fsS -X PUT -H "X-Samosa-Token: $TOKEN" -H "Origin: $ORIGIN" \
  -H 'Content-Type: application/json' -d '{"name":"  <b>Deepan</b>  "}' "http://127.0.0.1:$PORT/v1/profile")
printf '%s' "$PUT_RESP" | grep -q '"name":"<b>Deepan</b>"' || {
  echo "FAIL: name was not trimmed or was not preserved as literal text"; echo "$PUT_RESP"; exit 1;
}

STATUS_JSON=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H "Origin: $ORIGIN" "http://127.0.0.1:$PORT/v1/setup/status")
printf '%s' "$STATUS_JSON" | grep -q '"next_step":"welcome"' || { echo "FAIL: expected next_step welcome"; echo "$STATUS_JSON"; exit 1; }

# --- welcome completion is idempotent and doesn't reset on name edits ---
WELCOME_1=$(curl -fsS -X POST -H "X-Samosa-Token: $TOKEN" -H "Origin: $ORIGIN" "http://127.0.0.1:$PORT/v1/setup/welcome/complete")
COMPLETED_AT=$(printf '%s' "$WELCOME_1" | sed -n 's/.*"welcome_completed_at":"\([^"]*\)".*/\1/p')
[ -n "$COMPLETED_AT" ] || { echo "FAIL: welcome_completed_at missing"; echo "$WELCOME_1"; exit 1; }
sleep 1.1
WELCOME_2=$(curl -fsS -X POST -H "X-Samosa-Token: $TOKEN" -H "Origin: $ORIGIN" "http://127.0.0.1:$PORT/v1/setup/welcome/complete")
printf '%s' "$WELCOME_2" | grep -q "\"welcome_completed_at\":\"$COMPLETED_AT\"" || {
  echo "FAIL: welcome_completed_at changed on a second completion call (must be idempotent)"; echo "$WELCOME_2"; exit 1;
}
# Editing the name afterward must not reset welcome_completed_at.
curl -fsS -X PUT -H "X-Samosa-Token: $TOKEN" -H "Origin: $ORIGIN" \
  -H 'Content-Type: application/json' -d '{"name":"Renamed"}' "http://127.0.0.1:$PORT/v1/profile" >/dev/null
PROFILE_AFTER_RENAME=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H "Origin: $ORIGIN" "http://127.0.0.1:$PORT/v1/profile")
printf '%s' "$PROFILE_AFTER_RENAME" | grep -q "\"welcome_completed_at\":\"$COMPLETED_AT\"" || {
  echo "FAIL: renaming reset welcome_completed_at"; echo "$PROFILE_AFTER_RENAME"; exit 1;
}
printf '%s' "$PROFILE_AFTER_RENAME" | grep -q '"name":"Renamed"' || { echo "FAIL: rename did not take effect"; exit 1; }

# --- setup status: no model installed -> "model", never "chat" ---
STATUS_JSON=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H "Origin: $ORIGIN" "http://127.0.0.1:$PORT/v1/setup/status")
printf '%s' "$STATUS_JSON" | grep -q '"next_step":"model"' || { echo "FAIL: expected next_step model with no backend installed"; echo "$STATUS_JSON"; exit 1; }
printf '%s' "$STATUS_JSON" | grep -q '"profile_complete":false' || { echo "FAIL: profile_complete should be false pre-model"; exit 1; }

# --- corrupt profile.json yields a recoverable state, not a crash ---
printf 'not valid json{{{' >"$HOME_DIR/profile.json"
STATUS=$(curl -sS -o "$TMP/resp.json" -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" -H "Origin: $ORIGIN" "http://127.0.0.1:$PORT/v1/setup/status")
[ "$STATUS" = "200" ] || { echo "FAIL: corrupt profile.json should still yield 200 setup/status, got $STATUS"; cat "$TMP/resp.json"; exit 1; }
grep -q '"next_step":"name"' "$TMP/resp.json" || { echo "FAIL: corrupt profile should recover to next_step name"; cat "$TMP/resp.json"; exit 1; }

echo "test_profile_setup.sh: PASS"

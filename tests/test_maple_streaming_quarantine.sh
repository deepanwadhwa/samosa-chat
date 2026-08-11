#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
GATEWAY="$ROOT/build/samosa-gateway"
[ -x "$GATEWAY" ] || { echo "FAIL: build/samosa-gateway is missing" >&2; exit 1; }

TMP=$(mktemp -d)
GATEWAY_PID=""
cleanup() {
  if [ -n "$GATEWAY_PID" ]; then
    kill "$GATEWAY_PID" 2>/dev/null || true
    wait "$GATEWAY_PID" 2>/dev/null || true
  fi
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

MAPLE_DIR="$TMP/home/models/maple"
mkdir -p "$MAPLE_DIR"
printf '{}\n' >"$MAPLE_DIR/config.json"
printf '{}\n' >"$MAPLE_DIR/tokenizer.json"
printf '{}\n' >"$MAPLE_DIR/model.safetensors.index.json"
printf '#!/bin/sh\nexit 0\n' >"$TMP/samosa-maple"
chmod +x "$TMP/samosa-maple"

PORT=$((18000 + ($$ % 10000)))
SAMOSA_HOME="$TMP/home" \
SAMOSA_PORT="$PORT" \
SAMOSA_BACKEND_PORT=$((PORT + 1)) \
SAMOSA_MAPLE_ENGINE="$TMP/samosa-maple" \
SAMOSA_QWEN_ENGINE="$TMP/missing-qwen" \
SAMOSA_QWEN_MODEL="$TMP/missing-qwen-model" \
SAMOSA_BONSAI_SERVER="$TMP/missing-llama" \
SAMOSA_BONSAI_MODEL="$TMP/missing-bonsai" \
SAMOSA_ORNITH_MODEL="$TMP/missing-ornith" \
  "$GATEWAY" >"$TMP/gateway.log" 2>&1 &
GATEWAY_PID=$!

i=0
while [ "$i" -lt 50 ]; do
  if curl -fsS --max-time 1 "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1; then break; fi
  i=$((i + 1))
  sleep 0.1
done
[ "$i" -lt 50 ] || { echo "FAIL: gateway did not start" >&2; cat "$TMP/gateway.log" >&2; exit 1; }

BACKENDS=$(curl -fsS --max-time 2 "http://127.0.0.1:$PORT/v1/backends")
printf '%s' "$BACKENDS" | grep -q '"id":"maple"[^}]*"available":false' || {
  echo "FAIL: legacy full-resident Maple assets were accepted" >&2
  printf '%s\n' "$BACKENDS" >&2
  exit 1
}

printf 'streamed experts fixture\n' >"$MAPLE_DIR/maple-experts.bin"
printf 'resident fixture\n' >"$MAPLE_DIR/maple-resident.safetensors"
printf '{}\n' >"$MAPLE_DIR/maple-manifest.json"

BACKENDS=$(curl -fsS --max-time 2 "http://127.0.0.1:$PORT/v1/backends")
printf '%s' "$BACKENDS" | grep -q '"id":"maple"[^}]*"available":true' || {
  echo "FAIL: complete streaming Maple assets were not accepted" >&2
  printf '%s\n' "$BACKENDS" >&2
  exit 1
}

echo "PASS: downloaded Maple path requires SSD-streaming artifacts"

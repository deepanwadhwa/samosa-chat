#!/bin/sh
set -eu

GATEWAY=${SAMOSA_COMPILED_GATEWAY:-./samosa-gateway}
BACKEND=${SAMOSA_FAKE_BACKEND:-./test_fake_openai_backend}
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
FS_SIDECAR=${SAMOSA_FS:-"$ROOT/build/samosa-fs"}
EXTRACTOR=${SAMOSA_EXTRACT:-"$ROOT/build/samosa-extract"}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/samosa-compiled-gateway.XXXXXX")
HOME_DIR="$TMP/home"
PORT=18977
BACKEND_PORT=18978
PID=""

cleanup() {
  [ -z "$PID" ] || kill "$PID" 2>/dev/null || true
  [ -z "$PID" ] || wait "$PID" 2>/dev/null || true
  /bin/rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$HOME_DIR/models/ornith-9b"
printf 'fixture\n' >"$HOME_DIR/models/ornith-9b/Ornith-1.0-9B-Q4_K_M.gguf"
printf 'ornith\n' >"$HOME_DIR/model-backend"
printf '<!doctype html><title>Compiled Samosa</title>\n' >"$TMP/app.html"
printf 'png\n' >"$TMP/logo.png"
/bin/mkdir "$TMP/files"
printf "Titli vaccination record, rabies booster 2026.\n" >"$TMP/files/cat-medical-note.txt"
/bin/mkdir "$TMP/slow"
printf '%s\n' '#!/bin/sh' \
  'last=""; for arg do last=$arg; done' \
  'case "$last" in' \
  '  */slow) printf "%s\\n" "$$" >"'$TMP'/slow-sidecar.pid"; exec /bin/sleep 30 ;;' \
  'esac' \
  'exec "'$FS_SIDECAR'" "$@"' >"$TMP/samosa-fs-wrapper"
/bin/chmod +x "$TMP/samosa-fs-wrapper"

# Deliberately expose no external executable through PATH. All utilities used
# below have absolute paths; the gateway/backend receive the same environment.
PATH="$TMP/no-python-bin"
/bin/mkdir "$PATH"
export PATH
if command -v python3 >/dev/null 2>&1; then
  echo "compiled gateway test PATH unexpectedly contains python3" >&2
  exit 1
fi

SAMOSA_HOME="$HOME_DIR" \
SAMOSA_PORT="$PORT" \
SAMOSA_BACKEND_PORT="$BACKEND_PORT" \
SAMOSA_APP_HTML="$TMP/app.html" \
SAMOSA_APP_LOGO="$TMP/logo.png" \
SAMOSA_BONSAI_SERVER="$BACKEND" \
SAMOSA_ORNITH_MODEL="$HOME_DIR/models/ornith-9b/Ornith-1.0-9B-Q4_K_M.gguf" \
SAMOSA_FS="$TMP/samosa-fs-wrapper" \
SAMOSA_EXTRACT="$EXTRACTOR" \
"$GATEWAY" >"$TMP/gateway.log" 2>&1 &
PID=$!

i=0
while [ "$i" -lt 100 ]; do
  health=$(/usr/bin/curl -fsS "http://127.0.0.1:$PORT/healthz" 2>/dev/null || true)
  printf '%s' "$health" | /usr/bin/grep -q '"ready":true' && break
  kill -0 "$PID" 2>/dev/null || { /bin/cat "$TMP/gateway.log" >&2; exit 1; }
  /bin/sleep 0.05
  i=$((i + 1))
done
printf '%s' "$health" | /usr/bin/grep -q '"compiled":true'
printf '%s' "$health" | /usr/bin/grep -q '"ready":true'

reply=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
  -H 'Content-Type: application/json' \
  --data-binary '{"messages":[{"role":"user","content":"hello"}],"stream":false}')
printf '%s' "$reply" | /usr/bin/grep -q 'compiled reply'

report=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/run" \
  -H 'Content-Type: application/json' \
  --data-binary "{\"goal\":\"report what is here\",\"folder\":\"$TMP/files\"}")
printf '%s' "$report" | /usr/bin/grep -q '"type":"report"'
printf '%s' "$report" | /usr/bin/grep -q '"type":"done"'

find=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/run" \
  -H 'Content-Type: application/json' \
  --data-binary "{\"goal\":\"find my cat medical record\",\"folder\":\"$TMP/files\"}")
printf '%s' "$find" | /usr/bin/grep -q '"tool":"fs_read_text"'
printf '%s' "$find" | /usr/bin/grep -q "Found the matching record at cat-medical-note.txt"
if printf '%s' "$find" | /usr/bin/grep -q 'samosa_tool'; then
  echo "compiled find leaked tool protocol" >&2
  exit 1
fi

/usr/bin/curl -sS -X POST "http://127.0.0.1:$PORT/v1/jobs/run" \
  -H 'Content-Type: application/json' \
  --data-binary "{\"goal\":\"report what is here\",\"folder\":\"$TMP/slow\"}" \
  >"$TMP/slow-result" 2>/dev/null &
SLOW_CURL=$!
i=0
while [ "$i" -lt 100 ] && [ ! -s "$TMP/slow-sidecar.pid" ]; do /bin/sleep 0.02; i=$((i + 1)); done
[ -s "$TMP/slow-sidecar.pid" ]
SIDE_PID=$(/bin/cat "$TMP/slow-sidecar.pid")
/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/kill" >/dev/null
wait "$SLOW_CURL" 2>/dev/null || true
if /bin/kill -0 "$SIDE_PID" 2>/dev/null; then
  echo "kill route left a Jobs sidecar running" >&2
  exit 1
fi
wait "$PID"
PID=""
echo "compiled gateway without python: PASS"

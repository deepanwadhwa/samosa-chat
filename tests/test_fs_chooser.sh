#!/bin/sh
set -eu

fail() {
  echo "$(basename "$0"): FAIL: $1" >&2
  exit 1
}

# T1.3 (docs/TASKS_UI_CHUTNI.md sec5.4): safe browser directory chooser.
# HOME is overridden to a sandboxed fixture tree -- this is the real OS user
# home the chooser roots from (see Gateway.user_home), distinct from
# SAMOSA_HOME (Samosa's own app-state directory, also overridden here).

BUILD_DIR="${BUILD_DIR:-build}"
GATEWAY="${SAMOSA_COMPILED_GATEWAY:-./$BUILD_DIR/samosa-gateway}"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/fs_chooser_test.XXXXXX")
HOME_DIR="$TMP/home"
SAMOSA_HOME="$TMP/samosahome"
PORT=18986
PID=""

cleanup() {
  [ -z "$PID" ] || kill "$PID" 2>/dev/null || true
  [ -z "$PID" ] || wait "$PID" 2>/dev/null || true
  chmod 755 "$HOME_DIR/Documents/NoPerm" 2>/dev/null || true
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM



mkdir -p "$HOME_DIR/Documents/Research" "$HOME_DIR/.hidden_dir" "$HOME_DIR/Documents/NoPerm" "$SAMOSA_HOME"
printf 'not a folder\n' >"$HOME_DIR/Documents/notes.txt"
chmod 000 "$HOME_DIR/Documents/NoPerm"
ln -s /etc "$HOME_DIR/Documents/EvilLink"
printf '<!doctype html><title>Compiled Samosa</title><meta name="samosa-ui-token" content="__SAMOSA_UI_TOKEN__">\n' >"$TMP/app.html"
printf 'png\n' >"$TMP/logo.png"

HOME="$HOME_DIR" \
SAMOSA_HOME="$SAMOSA_HOME" \
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

TOKEN=$(cat "$SAMOSA_HOME/run/ui-token")
ORIGIN="http://127.0.0.1:$PORT"
HOME_REAL=$(cd "$HOME_DIR" && pwd -P)

encode() { python3 -c "import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1], safe=''))" "$1"; }

# --- auth: no route here works without the per-launch UI token ---
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/v1/fs/roots")
[ "$STATUS" = "401" ] || { echo "FAIL: /v1/fs/roots without a token should be 401, got $STATUS"; exit 1; }
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/v1/fs/directories?path=%2F")
[ "$STATUS" = "401" ] || { echo "FAIL: /v1/fs/directories without a token should be 401, got $STATUS"; exit 1; }

# --- roots: Home is present, canonical, and marked readable/connected ---
ROOTS_JSON=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H "Origin: $ORIGIN" "http://127.0.0.1:$PORT/v1/fs/roots")
python3 -c "
import json, sys
data = json.loads(sys.argv[1])
roots = data['roots']
home = [r for r in roots if r['kind'] == 'home']
assert len(home) == 1, f'expected exactly one home root, got {home}'
r = home[0]
assert r['path'] == sys.argv[2], f'home root path {r[\"path\"]!r} != real home {sys.argv[2]!r}'
assert r['readable'] is True and r['connected'] is True
assert r['chooser_root_id'], 'chooser_root_id must be non-empty'
for key in ('chooser_root_id', 'label', 'path', 'kind', 'volume_identity', 'readable', 'connected'):
    assert key in r, f'missing locked field {key!r}'
" "$ROOTS_JSON" "$HOME_REAL"

# --- directories at Home: subfolder listed, dotfile excluded, no file leakage ---
ENC_HOME=$(encode "$HOME_REAL")
HOME_LIST=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H "Origin: $ORIGIN" "http://127.0.0.1:$PORT/v1/fs/directories?path=$ENC_HOME")
python3 -c "
import json, sys
data = json.loads(sys.argv[1])
names = [d['name'] for d in data['directories']]
assert 'Documents' in names, names
assert '.hidden_dir' not in names, 'hidden directories must be excluded by default'
assert data['parent'] is None, 'the home root itself must report parent:null (chooser cannot go above a root)'
assert data['chooser_root_id'], 'chooser_root_id must be echoed back'
" "$HOME_LIST"

# --- directories at Documents: readable + denied entries, symlink and file excluded ---
ENC_DOCS=$(encode "$HOME_REAL/Documents")
DOCS_LIST=$(curl -fsS -H "X-Samosa-Token: $TOKEN" -H "Origin: $ORIGIN" "http://127.0.0.1:$PORT/v1/fs/directories?path=$ENC_DOCS")
python3 -c "
import json, sys
data = json.loads(sys.argv[1])
by_name = {d['name']: d for d in data['directories']}
assert by_name['Research']['readable'] is True
assert by_name['NoPerm']['readable'] is False, 'a permission-denied directory must be listed as unavailable, not omitted'
assert 'EvilLink' not in by_name, 'a symlink must never be listed as an enterable directory'
assert 'notes.txt' not in by_name, 'the API must return no file contents/entries, directories only'
assert data['parent'] == sys.argv[2]
" "$DOCS_LIST" "$HOME_REAL"

# --- denied child directory cannot itself be entered (403, not a crash) ---
ENC_NOPERM=$(encode "$HOME_REAL/Documents/NoPerm")
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" -H "Origin: $ORIGIN" "http://127.0.0.1:$PORT/v1/fs/directories?path=$ENC_NOPERM")
[ "$STATUS" = "403" ] || { echo "FAIL: entering a permission-denied directory should be 403, got $STATUS"; exit 1; }

# --- symlink escape: browsing straight at the symlink itself is denied ---
ENC_EVIL=$(encode "$HOME_REAL/Documents/EvilLink")
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" -H "Origin: $ORIGIN" "http://127.0.0.1:$PORT/v1/fs/directories?path=$ENC_EVIL")
[ "$STATUS" = "403" ] || { echo "FAIL: a symlink escaping the root should be 403 path_denied, got $STATUS"; exit 1; }

# --- traversal: a path fully outside every allowed root is denied, not served ---
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" -H "Origin: $ORIGIN" "http://127.0.0.1:$PORT/v1/fs/directories?path=%2Fetc")
[ "$STATUS" = "403" ] || { echo "FAIL: /etc (outside every root) should be 403 path_denied, got $STATUS"; exit 1; }

# --- encoded traversal: the wire bytes carry ".." as %2e%2e (not literal dots),
# the classic evasion against a filter that string-matches ".." before
# decoding. This must decode-then-validate identically to the plain case
# above, not be smuggled past the containment check. Built by hand (not via
# encode(), which would double-encode the literal '%' already present).
TRAVERSAL_QUERY="$HOME_REAL/Documents/%2e%2e/%2e%2e/%2e%2e/%2e%2e/%2e%2e/%2e%2e/%2e%2e/%2e%2e/%2e%2e/%2e%2e/etc"
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" -H "Origin: $ORIGIN" "http://127.0.0.1:$PORT/v1/fs/directories?path=$TRAVERSAL_QUERY")
[ "$STATUS" = "403" ] || { echo "FAIL: encoded (%2e%2e) traversal outside every root should be 403 path_denied, got $STATUS"; exit 1; }

# --- missing/malformed requests fail closed, not with a crash ---
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" -H "Origin: $ORIGIN" "http://127.0.0.1:$PORT/v1/fs/directories")
[ "$STATUS" = "400" ] || { echo "FAIL: missing path query param should be 400, got $STATUS"; exit 1; }

ENC_MISSING=$(encode "$HOME_REAL/Documents/DoesNotExist")
STATUS=$(curl -sS -o /dev/null -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" -H "Origin: $ORIGIN" "http://127.0.0.1:$PORT/v1/fs/directories?path=$ENC_MISSING")
[ "$STATUS" = "404" ] || { echo "FAIL: a nonexistent directory should be 404, got $STATUS"; exit 1; }

# --- the gateway must still be alive after all of the above (no crash) ---
curl -fsS "http://127.0.0.1:$PORT/healthz" >/dev/null || { echo "FAIL: gateway did not survive the chooser test"; exit 1; }

echo "test_fs_chooser.sh: PASS"

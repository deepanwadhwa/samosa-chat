#!/bin/sh
set -eu

# Phase W (docs/TASKS_WEB_SEARCH.md) W7. Drives the real compiled gateway
# against a fake backend and a fake curl, with no network access of any kind:
#
#  - the provider host is 203.0.113.10, a TEST-NET-3 documentation address.
#    getaddrinfo() parses it numerically, so resolve_public_host() runs for
#    real without a DNS server, and the address is guaranteed never routable.
#  - SAMOSA_CURL points at a shell stub, so the real curl_request() code path
#    runs (config-file generation, argv construction, response parsing) while
#    nothing leaves the machine. That stub is also what makes the credential
#    test possible: it records its own argv, which is exactly what a local
#    attacker reading `ps` would see.
#  - page reading uses the existing SAMOSA_WEB_STUB_DIR seam.
#
# What this cannot do is prove a real provider's request/response shape. Only
# a machine holding that provider's credentials can, and the acceptance list
# in docs/TASKS_WEB_SEARCH.md requires that distinction to be stated.

BUILD_DIR="${BUILD_DIR:-build}"
GATEWAY="${SAMOSA_COMPILED_GATEWAY:-./$BUILD_DIR/samosa-gateway}"
BACKEND="${SAMOSA_FAKE_BACKEND:-./$BUILD_DIR/test_fake_openai_backend}"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/web_search_test.XXXXXX")
HOME_DIR="$TMP/home"
# Distinct from every other tests/*.sh port (18642-18643, 18977-18998, 19010,
# 19020-19021). PORT+1 is this run's backend port.
PORT=19030
GW_PID=""

cleanup() {
  [ -z "$GW_PID" ] || kill "$GW_PID" 2>/dev/null || true
  [ -z "$GW_PID" ] || wait "$GW_PID" 2>/dev/null || true
  curl -sS -m 2 -X POST "http://127.0.0.1:$((PORT + 1))/shutdown" >/dev/null 2>&1 || true
  rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

fail() { echo "test_web_search.sh: $1" >&2; exit 1; }

mkdir -p "$HOME_DIR/qwen-model" "$TMP/stub" "$TMP/curl"
printf '<!doctype html><title>Compiled Samosa</title>\n' >"$TMP/app.html"
printf 'png\n' >"$TMP/logo.png"
printf 'experts-fixture\n' >"$HOME_DIR/qwen-model/experts.bin"
printf 'tokenizer-fixture\n' >"$TMP/tokenizer.json"

# --- page fixtures for the SAMOSA_WEB_STUB_DIR transport seam ---------------
printf '<html><head><title>Careers</title></head><body><script>secret()</script><h1>Roles</h1><p>Engineer &amp; Designer</p></body></html>' \
  >"$TMP/stub/http-example-com-jobs.html"
printf 'User-agent: *\nAllow: /\n' >"$TMP/stub/robots.txt"

# --- fake curl -------------------------------------------------------------
# Reads the options curl would have read, records everything for inspection,
# and writes the canned provider response to the configured output path.
cat >"$TMP/curl/fake-curl" <<'SH'
#!/bin/sh
# Record the argument vector exactly as a local `ps` would show it.
printf '%s\n' "$*" >>"$FAKE_CURL_ARGV"
conf=""
prev=""
for a in "$@"; do
  [ "$prev" = "--config" ] && conf="$a"
  prev="$a"
done
[ -n "$conf" ] || { echo "fake-curl: no --config" >&2; exit 2; }
cat "$conf" >>"$FAKE_CURL_CONFIG"
ls -l "$conf" | cut -c1-10 >>"$FAKE_CURL_MODE"
value() { sed -n "s/^$1 = \"\\(.*\\)\"\$/\\1/p" "$conf" | tail -1; }
out=$(value output)
head=$(value dump-header)
body=$(value data-binary)
case "$body" in
  @*) cat "${body#@}" >>"$FAKE_CURL_BODY" ;;
esac
[ -z "$head" ] || printf 'HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n' >"$head"
[ -z "$out" ] || cat "$FAKE_CURL_RESPONSE" >"$out"
printf '200'
SH
chmod +x "$TMP/curl/fake-curl"
export FAKE_CURL_ARGV="$TMP/curl/argv.txt"
export FAKE_CURL_CONFIG="$TMP/curl/config.txt"
export FAKE_CURL_MODE="$TMP/curl/mode.txt"
export FAKE_CURL_BODY="$TMP/curl/body.txt"
export FAKE_CURL_RESPONSE="$TMP/curl/response.json"
: >"$FAKE_CURL_ARGV"; : >"$FAKE_CURL_CONFIG"; : >"$FAKE_CURL_MODE"; : >"$FAKE_CURL_BODY"
cat >"$FAKE_CURL_RESPONSE" <<'JSON'
{"web":{"results":[
  {"title":"Careers at Example","url":"http://example.com/jobs","description":"Open roles."},
  {"title":"Bad scheme","url":"javascript:alert(1)","description":"must be dropped"},
  {"title":"Second","url":"https://example.org/two","description":"Another result."}
]}}
JSON

SECRET_KEY="brave-secret-key-do-not-leak-12345"

# SAMOSA_WEB_STUB_DIR replaces the transport with local files *before* the
# resolver runs, so a run with the seam on cannot exercise the SSRF block
# list at all -- a blocked address just reports "no stub". Section 2
# therefore runs against a gateway with the seam off, which needs no network:
# every case there is rejected on scheme, port, credentials, or a numeric
# address getaddrinfo() parses without consulting DNS.
# Args: [stub|nostub] [offline]
start_gateway() {
  SAMOSA_HOME="$HOME_DIR"
  SAMOSA_PORT="$PORT"
  SAMOSA_BACKEND_PORT=$((PORT + 1))
  SAMOSA_APP_HTML="$TMP/app.html"
  SAMOSA_APP_LOGO="$TMP/logo.png"
  SAMOSA_QWEN_ENGINE="$BACKEND"
  SAMOSA_QWEN_MODEL="$HOME_DIR/qwen-model"
  SAMOSA_TOKENIZER="$TMP/tokenizer.json"
  SAMOSA_WEB_MIN_INTERVAL=0
  SAMOSA_CURL="$TMP/curl/fake-curl"
  export SAMOSA_HOME SAMOSA_PORT SAMOSA_BACKEND_PORT SAMOSA_APP_HTML SAMOSA_APP_LOGO \
         SAMOSA_QWEN_ENGINE SAMOSA_QWEN_MODEL SAMOSA_TOKENIZER SAMOSA_WEB_MIN_INTERVAL SAMOSA_CURL
  if [ "${1:-stub}" = "stub" ]; then
    SAMOSA_WEB_STUB_DIR="$TMP/stub"; export SAMOSA_WEB_STUB_DIR
  else
    unset SAMOSA_WEB_STUB_DIR || true
  fi
  if [ "${2:-}" = "offline" ]; then
    SAMOSA_OFFLINE=1; export SAMOSA_OFFLINE
  else
    unset SAMOSA_OFFLINE || true
  fi
  "$GATEWAY" >"$TMP/stdout.log" 2>"$TMP/stderr.log" &
  GW_PID=$!
  i=0
  while [ "$i" -lt 100 ]; do
    curl -fsS "http://127.0.0.1:$PORT/healthz" 2>/dev/null | grep -q '"ready":true' && break
    sleep 0.05; i=$((i + 1))
  done
  TOKEN=$(cat "$HOME_DIR/run/ui-token")
}
stop_gateway() {
  [ -z "$GW_PID" ] || kill "$GW_PID" 2>/dev/null || true
  [ -z "$GW_PID" ] || wait "$GW_PID" 2>/dev/null || true
  GW_PID=""
  curl -sS -m 2 -X POST "http://127.0.0.1:$((PORT + 1))/shutdown" >/dev/null 2>&1 || true
  sleep 0.2
}
auth() { curl -sS -H "X-Samosa-Token: $TOKEN" "$@"; }
status_of() { curl -sS -o /dev/null -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" "$@"; }

# ===========================================================================
# 1. A fresh install (WK): the keyless default is ready but not yet allowed.
#
# This is the state every new user starts in, so it is asserted precisely:
# search is *configured* (a request can be built with no credential at all --
# that is what WK1 bought) and simultaneously *not allowed* (nobody has
# answered the question yet -- that is what WK2 costs). The two are separate
# fields because only one of them is a problem the user can fix in an editor.
# ===========================================================================
start_gateway nostub
CFG=$(auth "http://127.0.0.1:$PORT/v1/web/config")
printf '%s' "$CFG" | grep -q '"provider":"parallel"' || fail "the keyless provider must be the default"
printf '%s' "$CFG" | grep -q '"keyless":true' || fail "the default provider must report keyless"
printf '%s' "$CFG" | grep -q '"search_configured":true' || fail "the keyless default must build without a credential"
printf '%s' "$CFG" | grep -q '"consent":"unset"' || fail "a fresh install must report consent unset"
printf '%s' "$CFG" | grep -q '"web_available":false' || fail "web must not be available before consent"
printf '%s' "$CFG" | grep -q '"fetch_available":false' || fail "fetch must wait for consent too"

# The routes are new, so they must be token-gated (T1.2 fail-closed default).
[ "$(curl -sS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/v1/web/config")" = "401" ] \
  || fail "/v1/web/config must require the UI token"
[ "$(curl -sS -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/v1/web/search" \
     -H 'Content-Type: application/json' --data '{"query":"x"}')" = "401" ] \
  || fail "/v1/web/search must require the UI token"
[ "$(curl -sS -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/v1/web/consent" \
     -H 'Content-Type: application/json' --data '{"granted":true}')" = "401" ] \
  || fail "/v1/web/consent must require the UI token"

# --- Nothing reaches the network before the question is answered -----------
: >"$FAKE_CURL_ARGV"
S=$(auth -X POST "http://127.0.0.1:$PORT/v1/web/search" -H 'Content-Type: application/json' --data '{"query":"x"}')
printf '%s' "$S" | grep -q 'consent_required' || fail "search before consent must report consent_required"
F=$(auth -X POST "http://127.0.0.1:$PORT/v1/web/fetch" -H 'Content-Type: application/json' \
     --data '{"url":"http://example.com/jobs"}')
printf '%s' "$F" | grep -q 'consent_required' || fail "fetch before consent must report consent_required"
[ ! -s "$FAKE_CURL_ARGV" ] || fail "a request was sent before consent was given"

# ===========================================================================
# 1b. WK2: the one-time question is recorded, and both answers stick.
# ===========================================================================
# A pre-existing config proves the merge: consent must not eat the user's key.
cat >"$HOME_DIR/config.json" <<JSON
{"offline":false,"search":{"provider":"brave","providers":{"brave":{"api_key":"$SECRET_KEY"}}}}
JSON
auth -X POST "http://127.0.0.1:$PORT/v1/web/consent" -H 'Content-Type: application/json' \
  --data '{"granted":false}' >/dev/null
grep -q '"consent":"denied"' "$HOME_DIR/config.json" || fail "a denial was not persisted"
grep -q "$SECRET_KEY" "$HOME_DIR/config.json" || fail "recording consent destroyed the user's API key"
grep -q '"offline":false' "$HOME_DIR/config.json" || fail "recording consent dropped an unrelated top-level key"
python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$HOME_DIR/config.json" \
  || fail "recording consent produced malformed JSON"
[ "$(status_of -X POST "http://127.0.0.1:$PORT/v1/web/search" -H 'Content-Type: application/json' \
     --data '{"query":"x"}')" = "403" ] || fail "a denied install must refuse search"
printf '%s' "$(auth -X POST "http://127.0.0.1:$PORT/v1/web/search" -H 'Content-Type: application/json' \
  --data '{"query":"x"}')" | grep -q 'consent_denied' || fail "denied must be distinct from unset"

# Denial is reversible from the UI, and granting it is what opens the routes.
auth -X POST "http://127.0.0.1:$PORT/v1/web/consent" -H 'Content-Type: application/json' \
  --data '{"granted":true}' >/dev/null
grep -q '"consent":"granted"' "$HOME_DIR/config.json" || fail "consent was not persisted"
CFG=$(auth "http://127.0.0.1:$PORT/v1/web/config")
printf '%s' "$CFG" | grep -q '"consent":"granted"' || fail "config did not report the new consent"
printf '%s' "$CFG" | grep -q '"fetch_available":true' || fail "fetch must open once consent is given"
if printf '%s' "$CFG" | grep -q "$SECRET_KEY"; then fail "/v1/web/consent leaked the API key in its reply"; fi
# A malformed request must not silently flip anything.
[ "$(status_of -X POST "http://127.0.0.1:$PORT/v1/web/consent" -H 'Content-Type: application/json' \
     --data '{"granted":"yes"}')" = "400" ] || fail "a non-boolean consent must be rejected"
grep -q '"consent":"granted"' "$HOME_DIR/config.json" || fail "a rejected request changed the stored consent"

rm -f "$HOME_DIR/config.json"
auth -X POST "http://127.0.0.1:$PORT/v1/web/consent" -H 'Content-Type: application/json' \
  --data '{"granted":true}' >/dev/null
grep -q '"consent":"granted"' "$HOME_DIR/config.json" || fail "consent must create config.json when absent"

# ===========================================================================
# 2. SSRF and URL validation on POST /v1/web/fetch.
# ===========================================================================
ssrf() {
  auth -X POST "http://127.0.0.1:$PORT/v1/web/fetch" -H 'Content-Type: application/json' \
    --data "{\"url\":\"$1\"}"
}
printf '%s' "$(ssrf 'http://127.0.0.1/x')"                 | grep -q 'blocked non-public address' || fail "loopback not blocked"
printf '%s' "$(ssrf 'http://169.254.169.254/latest/meta-data/')" | grep -q 'blocked non-public address' || fail "cloud metadata not blocked"
printf '%s' "$(ssrf 'http://10.0.0.5/')"                   | grep -q 'blocked non-public address' || fail "RFC1918 not blocked"
printf '%s' "$(ssrf 'http://192.168.1.1/')"                | grep -q 'blocked non-public address' || fail "192.168/16 not blocked"
printf '%s' "$(ssrf 'http://100.64.0.1/')"                 | grep -q 'blocked non-public address' || fail "CGNAT not blocked"
printf '%s' "$(ssrf 'http://[::1]/')"                      | grep -q 'blocked non-public address' || fail "IPv6 loopback not blocked"
printf '%s' "$(ssrf 'http://[::ffff:127.0.0.1]/')"         | grep -q 'blocked non-public address' || fail "IPv4-mapped IPv6 not blocked"
printf '%s' "$(ssrf 'http://[fd00::1]/')"                  | grep -q 'blocked non-public address' || fail "unique-local IPv6 not blocked"
printf '%s' "$(ssrf 'http://[fe80::1]/')"                  | grep -q 'blocked non-public address' || fail "link-local IPv6 not blocked"
printf '%s' "$(ssrf 'http://2130706433/')"                 | grep -q 'blocked non-public address' || fail "decimal-encoded loopback not blocked"
printf '%s' "$(ssrf 'http://example.com:8080/')"           | grep -q 'non-standard URL ports are blocked' || fail "non-standard port not blocked"
printf '%s' "$(ssrf 'file:///etc/passwd')"                 | grep -q 'only public http'   || fail "file:// not blocked"
printf '%s' "$(ssrf 'gopher://example.com/')"              | grep -q 'only public http'   || fail "gopher:// not blocked"
printf '%s' "$(ssrf 'http://user:pass@example.com/')"      | grep -q 'credentials in URLs are not allowed' || fail "URL credentials not blocked"
# Not covered anywhere, and not claimed: DNS rebinding against a *hostname*
# whose second resolution is private. Proving that needs a controlled resolver
# (TASKS_INTERNET.md E-I2), which this repo does not have. The pinning that
# would defeat it is implemented -- curl is given --resolve and --max-redirs 0
# -- but implemented is not tested.

# --- Page reading needs the stub transport ---------------------------------
stop_gateway
start_gateway stub
F=$(auth -X POST "http://127.0.0.1:$PORT/v1/web/fetch" -H 'Content-Type: application/json' \
     --data '{"url":"http://example.com/jobs"}')
printf '%s' "$F" | grep -q '"title":"Careers"' || fail "fetch did not extract the page title"
printf '%s' "$F" | grep -q 'Engineer & Designer' || fail "fetch did not decode entities"
if printf '%s' "$F" | grep -q 'secret'; then fail "fetch leaked <script> text into the extract"; fi

# ===========================================================================
# 3. Configured provider: the declarative executor end to end.
# ===========================================================================
cat >"$HOME_DIR/config.json" <<JSON
{
  "search": {
    "consent": "granted",
    "provider": "brave",
    "providers": { "brave": { "api_key": "$SECRET_KEY",
                              "url": "http://203.0.113.10/res/v1/web/search?q={query}&count=8" } }
  }
}
JSON
# Config is re-read per request, so no restart is needed for this to take.
CFG=$(auth "http://127.0.0.1:$PORT/v1/web/config")
printf '%s' "$CFG" | grep -q '"search_configured":true' || fail "a complete provider must report configured"
printf '%s' "$CFG" | grep -q '"provider":"brave"' || fail "config must name the provider"
if printf '%s' "$CFG" | grep -q "$SECRET_KEY"; then fail "/v1/web/config leaked the API key"; fi

S=$(auth -X POST "http://127.0.0.1:$PORT/v1/web/search" -H 'Content-Type: application/json' \
     --data '{"query":"careers & roles"}')
printf '%s' "$S" | grep -q '"title":"Careers at Example"' || fail "dot-path web.results extraction failed"
printf '%s' "$S" | grep -q '"url":"https://example.org/two"' || fail "second result missing"
if printf '%s' "$S" | grep -q 'javascript:'; then fail "a javascript: result URL was not dropped"; fi

# Placeholders: {query} URL-encoded, {api_key} resolved from provider config.
grep -q 'q=careers%20%26%20roles' "$FAKE_CURL_CONFIG" || fail "query was not URL-encoded into the provider URL"
grep -q "X-Subscription-Token: $SECRET_KEY" "$FAKE_CURL_CONFIG" || fail "preset header did not resolve {api_key}"

# --- The credential must never be visible in the process table -------------
if grep -q "$SECRET_KEY" "$FAKE_CURL_ARGV"; then
  fail "the API key appeared in curl's argv, where any local process can read it"
fi
grep -q -- '--config' "$FAKE_CURL_ARGV" || fail "the transport did not use a curl config file"
# ...nor in any log the gateway wrote.
if grep -q "$SECRET_KEY" "$TMP/stdout.log" "$TMP/stderr.log"; then fail "the API key reached a log"; fi
# ...and the config file carrying it is owner-only.
grep -q '^-rw-------' "$FAKE_CURL_MODE" || fail "the curl config file holding the key was not 0600"
# curl must stay fully silent: its own error messages go to an inherited
# stderr (the gateway log) and can quote a URL whose query string holds the
# key, as the serpapi and google presets' do.
if grep -q 'show-error' "$FAKE_CURL_CONFIG"; then
  fail "curl was allowed to write diagnostics, which can quote a key-bearing URL into the log"
fi

# --- An unresolved placeholder is an error, never an empty credential ------
cat >"$HOME_DIR/config.json" <<'JSON'
{"search":{"consent":"granted","provider":"brave","providers":{"brave":{"url":"http://203.0.113.10/s?q={query}"}}}}
JSON
CFG=$(auth "http://127.0.0.1:$PORT/v1/web/config")
printf '%s' "$CFG" | grep -q '"search_configured":false' || fail "a provider missing api_key must not report configured"
# The message is JSON-escaped in the response body, hence \" around the name.
printf '%s' "$CFG" | grep -q 'needs a .*api_key.* value' || fail "the missing placeholder must be named"
: >"$FAKE_CURL_ARGV"
auth -X POST "http://127.0.0.1:$PORT/v1/web/search" -H 'Content-Type: application/json' \
  --data '{"query":"x"}' >/dev/null
[ ! -s "$FAKE_CURL_ARGV" ] || fail "a request was sent despite an unresolved credential placeholder"

# --- A provider URL is SSRF-checked like any other -------------------------
cat >"$HOME_DIR/config.json" <<'JSON'
{"search":{"consent":"granted","provider":"searxng","providers":{"searxng":{"base_url":"http://127.0.0.1"}}}}
JSON
S=$(auth -X POST "http://127.0.0.1:$PORT/v1/web/search" -H 'Content-Type: application/json' --data '{"query":"x"}')
printf '%s' "$S" | grep -q 'blocked non-public address' || fail "a loopback provider base_url was not blocked"
# A non-standard port in a provider URL is refused before resolution, the same
# way it is for a page fetch.
cat >"$HOME_DIR/config.json" <<'JSON'
{"search":{"consent":"granted","provider":"searxng","providers":{"searxng":{"base_url":"http://203.0.113.10:8888"}}}}
JSON
S=$(auth -X POST "http://127.0.0.1:$PORT/v1/web/search" -H 'Content-Type: application/json' --data '{"query":"x"}')
printf '%s' "$S" | grep -q 'non-standard URL ports are blocked' || fail "a non-standard provider port was not blocked"
# That error quotes a config key, so it also proves the error body stayed
# valid JSON once quotes entered the message (samosa_http.h escaping).
printf '%s' "$S" | python3 -c 'import json,sys; json.load(sys.stdin)' \
  || fail "an error message containing quotes produced malformed JSON"

cat >"$HOME_DIR/config.json" <<'JSON'
{"search":{"consent":"granted","provider":"searxng","providers":{"searxng":{"base_url":"http://169.254.169.254"}}}}
JSON
S=$(auth -X POST "http://127.0.0.1:$PORT/v1/web/search" -H 'Content-Type: application/json' --data '{"query":"x"}')
printf '%s' "$S" | grep -q 'blocked non-public address' || fail "a metadata-service provider base_url was not blocked"

# --- A POST provider sends a body, and its body-borne key is also hidden ---
cat >"$HOME_DIR/config.json" <<JSON
{"search":{"consent":"granted","provider":"tavily","providers":{"tavily":{"api_key":"$SECRET_KEY",
 "url":"http://203.0.113.10/search"}}}}
JSON
cat >"$FAKE_CURL_RESPONSE" <<'JSON'
{"results":[{"title":"Tavily hit","url":"https://example.net/t","content":"Body-mapped description."}]}
JSON
: >"$FAKE_CURL_ARGV"; : >"$FAKE_CURL_BODY"
S=$(auth -X POST "http://127.0.0.1:$PORT/v1/web/search" -H 'Content-Type: application/json' \
     --data '{"query":"quoted \"phrase\""}')
printf '%s' "$S" | grep -q '"title":"Tavily hit"' || fail "POST provider result not parsed"
printf '%s' "$S" | grep -q '"description":"Body-mapped description."' || fail "fields mapping (content->description) failed"
grep -q "\"api_key\":\"$SECRET_KEY\"" "$FAKE_CURL_BODY" || fail "POST body did not resolve {api_key}"
grep -q '\\"phrase\\"' "$FAKE_CURL_BODY" || fail "a quoted query was not JSON-escaped into the POST body"
if grep -q "$SECRET_KEY" "$FAKE_CURL_ARGV"; then fail "a body-borne API key appeared in argv"; fi

# ===========================================================================
# 3b. WK1: the keyless default preset, end to end.
#
# The response fixture is the *shape* observed from the real service on
# 2026-07-28 (docs/regressions/web-search/keyless-2026-07-28/), reduced to two
# results. That makes this a regression test for the shape: if Parallel changes
# `structuredContent` or renames `excerpts`, the live check in that report is
# what catches it, and this is what pins the behaviour we built against.
# ===========================================================================
cat >"$FAKE_CURL_RESPONSE" <<'JSON'
{"jsonrpc":"2.0","id":1,"result":{"content":[{"type":"text","text":"ignored rendering"}],
 "structuredContent":{"search_id":"search_abc","results":[
   {"url":"https://example.com/one","title":"First hit","publish_date":null,
    "excerpts":["Opening passage.","Second passage."]},
   {"url":"https://example.org/two","title":"Second hit","publish_date":null,
    "excerpts":["Only passage."]}],
  "warnings":null,"usage":[{"name":"sku_search","count":1}]},"isError":false}}
JSON
# The provider URL is the real https://search.parallel.ai/mcp, so point the
# preset at TEST-NET-3 for this run. Everything else -- body, dot-path, field
# mapping, {session_id} -- comes from the shipped preset, untouched.
cat >"$HOME_DIR/config.json" <<'JSON'
{"search":{"consent":"granted","provider":"parallel",
 "providers":{"parallel":{"url":"http://203.0.113.10/mcp"}}}}
JSON
: >"$FAKE_CURL_ARGV"; : >"$FAKE_CURL_BODY"; : >"$FAKE_CURL_CONFIG"
S=$(auth -X POST "http://127.0.0.1:$PORT/v1/web/search" -H 'Content-Type: application/json' \
     --data '{"query":"keyless probe"}')
printf '%s' "$S" | grep -q '"provider":"parallel"' || fail "the keyless provider did not run"
printf '%s' "$S" | grep -q '"title":"First hit"' || fail "result.structuredContent.results dot-path failed"
printf '%s' "$S" | grep -q '"url":"https://example.org/two"' || fail "second keyless result missing"
# The excerpts array is joined, not truncated to its first element: dropping the
# rest would throw away most of what makes these results answerable directly.
printf '%s' "$S" | grep -q 'Opening passage. … Second passage.' \
  || fail "the excerpts array was not joined into the description"
# A single-element array must not gain a separator.
printf '%s' "$S" | grep -q '"description":"Only passage."' || fail "a one-element excerpts array was mangled"

# The JSON-RPC envelope is built from the preset, and {session_id} resolves to
# a runtime value rather than erroring as an unresolved placeholder would.
grep -q '"method":"tools/call"' "$FAKE_CURL_BODY" || fail "the MCP envelope was not sent"
grep -q '"name":"web_search"' "$FAKE_CURL_BODY" || fail "the MCP tool name was not sent"
grep -q '"objective":"keyless probe"' "$FAKE_CURL_BODY" || fail "{query} did not reach objective"
grep -q '"search_queries":\["keyless probe"\]' "$FAKE_CURL_BODY" || fail "{query} did not reach search_queries"
grep -q '"session_id":"[0-9a-f]\{32\}"' "$FAKE_CURL_BODY" || fail "{session_id} did not resolve to a random hex id"
if grep -q '{session_id}' "$FAKE_CURL_BODY"; then fail "{session_id} was sent literally"; fi
# ...and it is stable within a process, which is what the provider asks for.
FIRST_SESSION=$(grep -o '"session_id":"[0-9a-f]*"' "$FAKE_CURL_BODY" | head -1)
: >"$FAKE_CURL_BODY"
auth -X POST "http://127.0.0.1:$PORT/v1/web/search" -H 'Content-Type: application/json' \
  --data '{"query":"second probe"}' >/dev/null
[ "$(grep -o '"session_id":"[0-9a-f]*"' "$FAKE_CURL_BODY" | head -1)" = "$FIRST_SESSION" ] \
  || fail "the session id changed between calls in one process"
# No Authorization header is invented for a provider that has no credential.
if grep -qi 'header = "Authorization' "$FAKE_CURL_CONFIG"; then
  fail "the keyless provider sent an Authorization header"
fi

# ===========================================================================
# 3c. WK5: the keyless daily budget.
#
# The provider publishes no rate limit for anonymous use, so Samosa imposes its
# own. Two properties matter and are asserted separately: the cap actually
# stops requests going out, and it applies *only* to the free default -- a user
# who supplied their own key has their own quota, and rationing that would be
# us limiting something we do not pay for.
# ===========================================================================
cat >"$HOME_DIR/config.json" <<'JSON'
{"search":{"consent":"granted","daily_limit":2,"provider":"parallel",
 "providers":{"parallel":{"url":"http://203.0.113.10/mcp"}}}}
JSON
rm -f "$HOME_DIR/web-usage.json"
: >"$FAKE_CURL_ARGV"
auth -X POST "http://127.0.0.1:$PORT/v1/web/search" -H 'Content-Type: application/json' --data '{"query":"one"}' >/dev/null
auth -X POST "http://127.0.0.1:$PORT/v1/web/search" -H 'Content-Type: application/json' --data '{"query":"two"}' >/dev/null
[ "$(grep -c . "$FAKE_CURL_ARGV")" = "2" ] || fail "the first two searches under the cap did not go out"
CFG=$(auth "http://127.0.0.1:$PORT/v1/web/config")
printf '%s' "$CFG" | grep -q '"searches_today":2' || fail "the config did not report today's usage"
printf '%s' "$CFG" | grep -q '"daily_limit":2' || fail "the config did not report the cap"

: >"$FAKE_CURL_ARGV"
S=$(auth -X POST "http://127.0.0.1:$PORT/v1/web/search" -H 'Content-Type: application/json' --data '{"query":"three"}')
printf '%s' "$S" | grep -q 'free searches for today' || fail "the third search was not capped"
printf '%s' "$S" | grep -q 'search_daily_limit' || fail "the cap must have its own error code, not 'unconfigured'"
printf '%s' "$S" | grep -q 'config.json' || fail "the cap message must say how to lift it"
[ "$(status_of -X POST "http://127.0.0.1:$PORT/v1/web/search" -H 'Content-Type: application/json' --data '{"query":"three"}')" = "429" ] || fail "a capped search must be 429, not 409"
[ ! -s "$FAKE_CURL_ARGV" ] || fail "a capped search still reached the transport"

# A user's own key is not counted and not capped, even past the same limit.
cat >"$HOME_DIR/config.json" <<JSON
{"search":{"consent":"granted","daily_limit":2,"provider":"brave",
 "providers":{"brave":{"api_key":"$SECRET_KEY","url":"http://203.0.113.10/s?q={query}"}}}}
JSON
cat >"$FAKE_CURL_RESPONSE" <<'JSON'
{"web":{"results":[{"title":"Keyed","url":"https://example.com/k","description":"Still working."}]}}
JSON
: >"$FAKE_CURL_ARGV"
S=$(auth -X POST "http://127.0.0.1:$PORT/v1/web/search" -H 'Content-Type: application/json' --data '{"query":"keyed"}')
printf '%s' "$S" | grep -q '"title":"Keyed"' || fail "a keyed provider was capped by the keyless budget"
[ -s "$FAKE_CURL_ARGV" ] || fail "a keyed provider did not reach the transport"
printf '%s' "$(auth "http://127.0.0.1:$PORT/v1/web/config")" | grep -q '"searches_today":0' \
  || fail "a keyed search was counted against the free budget"

# daily_limit: 0 turns the cap off entirely.
cat >"$HOME_DIR/config.json" <<'JSON'
{"search":{"consent":"granted","daily_limit":0,"provider":"parallel",
 "providers":{"parallel":{"url":"http://203.0.113.10/mcp"}}}}
JSON
printf '{"date":"1970-01-01","count":999}\n' >"$HOME_DIR/web-usage.json"
cat >"$FAKE_CURL_RESPONSE" <<'JSON'
{"jsonrpc":"2.0","id":1,"result":{"structuredContent":{"results":[
  {"url":"https://example.com/u","title":"Uncapped","excerpts":["Still working."]}]}}}
JSON
S=$(auth -X POST "http://127.0.0.1:$PORT/v1/web/search" -H 'Content-Type: application/json' --data '{"query":"uncapped"}')
printf '%s' "$S" | grep -q '"title":"Uncapped"' || fail "daily_limit 0 did not disable the cap"
# A stale date resets the count rather than carrying yesterday's total forward.
cat >"$HOME_DIR/config.json" <<'JSON'
{"search":{"consent":"granted","daily_limit":2,"provider":"parallel",
 "providers":{"parallel":{"url":"http://203.0.113.10/mcp"}}}}
JSON
printf '{"date":"1970-01-01","count":999}\n' >"$HOME_DIR/web-usage.json"
S=$(auth -X POST "http://127.0.0.1:$PORT/v1/web/search" -H 'Content-Type: application/json' --data '{"query":"newday"}')
printf '%s' "$S" | grep -q '"title":"Uncapped"' || fail "yesterday's count was not reset"
# A corrupt counter must not lock the feature out.
printf 'not json at all' >"$HOME_DIR/web-usage.json"
S=$(auth -X POST "http://127.0.0.1:$PORT/v1/web/search" -H 'Content-Type: application/json' --data '{"query":"corrupt"}')
printf '%s' "$S" | grep -q '"title":"Uncapped"' || fail "a corrupt usage file blocked search"
rm -f "$HOME_DIR/web-usage.json"

# ===========================================================================
# 4. The model-decided tool loop.
# ===========================================================================
cat >"$HOME_DIR/config.json" <<JSON
{"search":{"consent":"granted","provider":"brave","providers":{"brave":{"api_key":"$SECRET_KEY",
 "url":"http://203.0.113.10/res/v1/web/search?q={query}"}}}}
JSON
cat >"$FAKE_CURL_RESPONSE" <<'JSON'
{"web":{"results":[{"title":"Careers at Example","url":"http://example.com/jobs","description":"Open roles."}]}}
JSON

# 4a. `web: true` -> planner picks open_url, evidence reaches the answering turn.
R=$(auth -X POST "http://127.0.0.1:$PORT/v1/chat/completions" -H 'Content-Type: application/json' \
     --data '{"model":"qwen3.6-35b-a3b","stream":true,"web":true,
              "messages":[{"role":"user","content":"web tool probe open"}]}')
printf '%s' "$R" | grep -q 'saw web evidence' || fail "open_url evidence never reached the answering turn"
printf '%s' "$R" | grep -q '"web_activity":"Reading http://example.com/jobs' || fail "no tool-activity event was streamed"
printf '%s' "$R" | grep -q 'Read \\"Careers\\"' || fail "no completion event for the page read"
# The decoy URL inside the <think> span must not have been fetched.
if printf '%s' "$R" | grep -q 'decoy.example'; then fail "a URL inside a <think> span was acted on"; fi

# 4b. `web: true` -> planner picks web_search, results reach the answering turn.
R=$(auth -X POST "http://127.0.0.1:$PORT/v1/chat/completions" -H 'Content-Type: application/json' \
     --data '{"model":"qwen3.6-35b-a3b","stream":true,"web":true,
              "messages":[{"role":"user","content":"web tool probe search"}]}')
printf '%s' "$R" | grep -q 'saw web evidence' || fail "search evidence never reached the answering turn"
printf '%s' "$R" | grep -q 'Searching the web for' || fail "no search activity event was streamed"
if printf '%s' "$R" | grep -q "$SECRET_KEY"; then fail "the API key reached the chat stream"; fi

# 4c. `web_urls` -> read exactly what the user pasted, no planner involved.
R=$(auth -X POST "http://127.0.0.1:$PORT/v1/chat/completions" -H 'Content-Type: application/json' \
     --data '{"model":"qwen3.6-35b-a3b","stream":true,
              "web_urls":["http://example.com/jobs"],
              "messages":[{"role":"user","content":"web tool probe pasted"}]}')
printf '%s' "$R" | grep -q 'saw web evidence' || fail "a pasted web_urls page never reached the turn"

# 4d. The gate: a turn that asks for neither is untouched.
R=$(auth -X POST "http://127.0.0.1:$PORT/v1/chat/completions" -H 'Content-Type: application/json' \
     --data '{"model":"qwen3.6-35b-a3b","stream":true,
              "messages":[{"role":"user","content":"web tool probe plain"}]}')
printf '%s' "$R" | grep -q 'missing web evidence' || fail "a non-web turn was given web evidence"
if printf '%s' "$R" | grep -q '"web_activity"'; then fail "a non-web turn emitted web activity events"; fi
# Byte-identical to a pre-W turn: same body, no SSE preamble, no added fields.
PLAIN=$(auth -X POST "http://127.0.0.1:$PORT/v1/chat/completions" -H 'Content-Type: application/json' \
     --data '{"model":"qwen3.6-35b-a3b","stream":true,
              "messages":[{"role":"user","content":"hello"}]}')
[ "$PLAIN" = '{"choices":[{"index":0,"finish_reason":"stop","message":{"role":"assistant","content":"compiled reply"}}]}' ] \
  || fail "a plain turn is no longer a byte-for-byte passthrough"

# 4f. WK6: a repeated tool argument is refused rather than re-sent.
# Regression for the defect the first real-model run exposed: Ornith 9B asked
# for the same 403ing URL twice, spending its last tool call on it. The planner
# here asks for the identical URL on every round; the page must be fetched
# exactly once no matter how many times it is asked for.
: >"$FAKE_CURL_ARGV"
R=$(auth -X POST "http://127.0.0.1:$PORT/v1/chat/completions" -H 'Content-Type: application/json' \
     --data '{"model":"qwen3.6-35b-a3b","stream":true,"web":true,
              "messages":[{"role":"user","content":"web tool probe repeat"}]}')
[ "$(printf '%s' "$R" | grep -c 'Reading http://example.com/jobs')" = "1" ] \
  || fail "a repeated open_url was fetched more than once"
printf '%s' "$R" | grep -q 'saw web evidence' || fail "the first read of the repeated URL did not reach the turn"

# 4e. WK2: an install that has not answered the question is a pre-W install.
# `web: true` must not merely fail politely -- it must cost nothing at all: no
# planner round trip, no transport call, and no added text in the reply. This
# is W5's gate re-asserted for the state every new install now starts in.
cat >"$HOME_DIR/config.json" <<'JSON'
{"search":{"provider":"parallel","providers":{"parallel":{"url":"http://203.0.113.10/mcp"}}}}
JSON
: >"$FAKE_CURL_ARGV"
R=$(auth -X POST "http://127.0.0.1:$PORT/v1/chat/completions" -H 'Content-Type: application/json' \
     --data '{"model":"qwen3.6-35b-a3b","stream":true,"web":true,
              "messages":[{"role":"user","content":"hello"}]}')
[ "$R" = '{"choices":[{"index":0,"finish_reason":"stop","message":{"role":"assistant","content":"compiled reply"}}]}' ] \
  || fail "web:true without consent was not byte-identical to a plain turn"
[ ! -s "$FAKE_CURL_ARGV" ] || fail "a turn without consent still reached the transport"

# A pasted URL is different: the user explicitly asked, so it says why it did
# not read the page instead of silently dropping it.
R=$(auth -X POST "http://127.0.0.1:$PORT/v1/chat/completions" -H 'Content-Type: application/json' \
     --data '{"model":"qwen3.6-35b-a3b","stream":true,
              "web_urls":["http://example.com/jobs"],
              "messages":[{"role":"user","content":"web tool probe pasted"}]}')
printf '%s' "$R" | grep -q 'not been allowed to reach the internet' \
  || fail "a pasted URL without consent must explain itself"
printf '%s' "$R" | grep -q 'missing web evidence' || fail "a pasted URL without consent still fetched"

# ===========================================================================
# 5. The offline kill switch.
# ===========================================================================
# 5a. via config.json
cat >"$HOME_DIR/config.json" <<JSON
{"offline":true,"search":{"consent":"granted","provider":"brave","providers":{"brave":{"api_key":"$SECRET_KEY",
 "url":"http://203.0.113.10/res/v1/web/search?q={query}"}}}}
JSON
CFG=$(auth "http://127.0.0.1:$PORT/v1/web/config")
printf '%s' "$CFG" | grep -q '"offline":true' || fail "offline was not reported"
printf '%s' "$CFG" | grep -q '"fetch_available":false' || fail "fetch must be unavailable offline"
printf '%s' "$CFG" | grep -q '"search_configured":false' || fail "search must be unavailable offline"
[ "$(status_of -X POST "http://127.0.0.1:$PORT/v1/web/search" -H 'Content-Type: application/json' --data '{"query":"x"}')" = "409" ] \
  || fail "search must refuse while offline"
printf '%s' "$(ssrf 'http://example.com/jobs')" | grep -q 'offline mode' || fail "fetch must refuse while offline"
# The Jobs public-input fetcher goes through the same choke point.
printf '%s' "$(auth -X POST "http://127.0.0.1:$PORT/v1/jobs/public-inputs/update" \
  -H 'Content-Type: application/json' --data '{"job_id":"w","urls":["http://example.com/jobs"]}')" \
  | grep -q 'offline mode' || fail "offline did not cover the Jobs public-input fetcher"
: >"$FAKE_CURL_ARGV"
auth -X POST "http://127.0.0.1:$PORT/v1/web/fetch" -H 'Content-Type: application/json' \
  --data '{"url":"http://example.com/jobs"}' >/dev/null
[ ! -s "$FAKE_CURL_ARGV" ] || fail "offline mode still invoked the transport"

# 5b. via the environment, which must win over a config that says otherwise
stop_gateway
cat >"$HOME_DIR/config.json" <<'JSON'
{"offline":false}
JSON
start_gateway stub offline
CFG=$(auth "http://127.0.0.1:$PORT/v1/web/config")
printf '%s' "$CFG" | grep -q '"offline":true' || fail "SAMOSA_OFFLINE=1 did not override the config file"
stop_gateway

echo "test_web_search.sh: PASS"

#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=${TMPDIR:-/tmp}/samosa-wrapper-test.$$
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
mkdir -p "$TMP/bin" "$TMP/model"
: >"$TMP/model/experts.bin"
: >"$TMP/tokenizer_qwen36.json"

cat >"$TMP/bin/qwen36b" <<'EOF'
#!/bin/sh
printf '%s\n' "$@"
printf 'context=%s\n' "${SAMOSA_CONTEXT_TOKENS:-}"
EOF
chmod +x "$TMP/bin/qwen36b"

# serve/app always launch the gateway now (docs/TASKS_UI_CHUTNI.md T1.0: the
# gateway is the mandatory browser control plane, not an opt-in with a raw
# qwen36b HTTP-serve fallback) -- it takes no argv, so the fake prints the
# env vars the wrapper is responsible for passing through.
cat >"$TMP/bin/samosa-gateway" <<'EOF'
#!/bin/sh
printf 'port=%s\n' "${SAMOSA_PORT:-}"
printf 'context=%s\n' "${SAMOSA_CONTEXT_TOKENS:-}"
printf 'chutni=%s\n' "${SAMOSA_CHUTNI_SERVICE:-}"
EOF
chmod +x "$TMP/bin/samosa-gateway"

run() {
  SAMOSA_DISABLE_LAUNCHD=1 SAMOSA_HOME="$TMP" SAMOSA_PORT=18642 \
    sh "$ROOT/dist/samosa" "$@"
}

direct=$(run "hello world")
printf '%s\n' "$direct" | grep -qx -- '--no-thinking'
printf '%s\n' "$direct" | grep -qx -- '8192'
if printf '%s\n' "$direct" | grep -q -- '--seed'; then
  echo "unseeded invocation unexpectedly passed --seed" >&2
  exit 1
fi

general=$(run --think --seed 11 "solve this")
if printf '%s\n' "$general" | grep -q -- '--no-thinking\|--thinking-code'; then
  echo "general thinking selected the wrong template" >&2
  exit 1
fi
printf '%s\n' "$general" | grep -qx -- '8192'
printf '%s\n' "$general" | grep -qx -- '11'
printf '%s\n' "$general" | grep -qx -- '1024'

code=$(run --think-code --max-tokens 4096 "build this")
printf '%s\n' "$code" | grep -qx -- '--thinking-code'
printf '%s\n' "$code" | grep -qx -- '4096'
printf '%s\n' "$code" | grep -qx -- '2048'

custom_budget=$(run --think --thinking-budget 333 "solve this")
printf '%s\n' "$custom_budget" | grep -qx -- '333'

custom_context=$(run --context-tokens 65536 "remember this")
printf '%s\n' "$custom_context" | grep -qx -- '--context-tokens'
printf '%s\n' "$custom_context" | grep -qx -- '65536'

serve=$(run serve --foreground)
printf '%s\n' "$serve" | grep -qx -- 'port=18642'
printf '%s\n' "$serve" | grep -qx -- 'context=auto'
printf '%s\n' "$serve" | grep -qx -- "chutni=$TMP/bin/chutni-mcp"

serve_custom=$(run serve --foreground --context-tokens 65536)
printf '%s\n' "$serve_custom" | grep -qx -- 'context=65536'

cat >"$TMP/fake-curl" <<'EOF'
#!/bin/sh
exit 0
EOF
cat >"$TMP/fake-open" <<'EOF'
#!/bin/sh
printf 'OPEN %s\n' "$1"
EOF
chmod +x "$TMP/fake-curl" "$TMP/fake-open"
app=$(SAMOSA_CURL="$TMP/fake-curl" SAMOSA_OPEN="$TMP/fake-open" run app)
printf '%s\n' "$app" | grep -qx -- 'http://127.0.0.1:18642'
printf '%s\n' "$app" | grep -qx -- 'OPEN http://127.0.0.1:18642'
already=$(SAMOSA_CURL="$TMP/fake-curl" run serve)
printf '%s\n' "$already" | grep -q -- 'Samosa server running independently at http://127.0.0.1:18642'
if printf '%s\n' "$already" | grep -q -- 'answer a question'; then
  echo "serve fell through to usage after reporting an existing server" >&2
  exit 1
fi
stopped=$(SAMOSA_CURL="$TMP/fake-curl" run serve --stop)
printf '%s\n' "$stopped" | grep -qx -- 'Samosa server stopped.'

if run --seed nope test >/dev/null 2>&1; then
  echo "invalid seed was accepted" >&2
  exit 1
fi
if run --context-tokens nope test >/dev/null 2>&1; then
  echo "invalid context capacity was accepted" >&2
  exit 1
fi

# Version reporting: an unpackaged checkout reads the VERSION file; the
# `version` subcommand and the -v/--version flags must all agree with it.
expected_version=$(sed -n '1p' "$ROOT/VERSION" | tr -d '[:space:]')
[ -n "$expected_version" ] || { echo "VERSION file is empty" >&2; exit 1; }
for form in "version" "--version" "-v"; do
  got=$(run "$form" | sed -n '1p')
  if [ "$got" != "samosa $expected_version" ]; then
    echo "samosa $form reported [$got], expected [samosa $expected_version]" >&2
    exit 1
  fi
done

# With no gateway binary at all, serve must fail with a clear message rather
# than silently falling back to a raw qwen36b HTTP server (the fallback this
# task removed).
NO_GATEWAY_TMP=${TMPDIR:-/tmp}/samosa-wrapper-test-no-gateway.$$
trap 'rm -rf "$TMP" "$NO_GATEWAY_TMP"' EXIT HUP INT TERM
mkdir -p "$NO_GATEWAY_TMP/bin"
cp "$TMP/bin/qwen36b" "$NO_GATEWAY_TMP/bin/qwen36b"
no_gateway_out=$(SAMOSA_HOME="$NO_GATEWAY_TMP" SAMOSA_PORT=18643 sh "$ROOT/dist/samosa" serve 2>&1) && {
  echo "serve unexpectedly succeeded with no gateway binary present" >&2
  exit 1
}
printf '%s\n' "$no_gateway_out" | grep -q 'samosa-gateway' ||
  { echo "missing-gateway error did not name samosa-gateway: $no_gateway_out" >&2; exit 1; }

echo "samosa wrapper: PASS"

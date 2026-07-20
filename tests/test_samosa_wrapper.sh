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

run() {
  SAMOSA_HOME="$TMP" sh "$ROOT/dist/samosa" "$@"
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

serve=$(run serve)
printf '%s\n' "$serve" | grep -qx -- '--serve'
printf '%s\n' "$serve" | grep -qx -- '8642'
printf '%s\n' "$serve" | grep -qx -- 'context=auto'

serve_custom=$(run serve --context-tokens 65536)
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
printf '%s\n' "$app" | grep -qx -- 'http://127.0.0.1:8642'
printf '%s\n' "$app" | grep -qx -- 'OPEN http://127.0.0.1:8642'
stopped=$(SAMOSA_CURL="$TMP/fake-curl" run serve --stop)
printf '%s\n' "$stopped" | grep -qx -- 'Samosa server stopped.'

cat >"$TMP/bin/samosa-gateway" <<'EOF'
#!/usr/bin/env python3
import os
import sys
print(" ".join(sys.argv[1:]))
print("home=" + os.environ["SAMOSA_HOME"])
EOF
chmod +x "$TMP/bin/samosa-gateway"
listed=$(run models)
printf '%s\n' "$listed" | grep -qx -- '--models'
printf '%s\n' "$listed" | grep -qx -- "home=$TMP"
pulled=$(run pull ornith)
printf '%s\n' "$pulled" | grep -qx -- '--pull ornith'
pulled_default=$(run pull)
printf '%s\n' "$pulled_default" | grep -qx -- '--pull qwen'
if run pull unknown >/dev/null 2>&1; then
  echo "unknown model download was accepted" >&2
  exit 1
fi

if run --seed nope test >/dev/null 2>&1; then
  echo "invalid seed was accepted" >&2
  exit 1
fi
if run --context-tokens nope test >/dev/null 2>&1; then
  echo "invalid context capacity was accepted" >&2
  exit 1
fi

echo "samosa wrapper: PASS"

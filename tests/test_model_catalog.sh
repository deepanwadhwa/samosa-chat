#!/bin/sh
set -eu

fail() {
  echo "$(basename "$0"): FAIL: $1" >&2
  exit 1
}

# T2.1 (docs/TASKS_UI_CHUTNI.md section 5.3): GET /v1/models/catalog.
# Two things are asserted: (1) the real bundled assets/models.json parses,
# validates, and serves correctly against the compiled gateway with no
# model installed (the normal CI state); (2) the handler's generic
# validation/detection/compatibility logic is exercised against small,
# fast, deterministic fixture catalogs and fixture files -- never the real
# multi-gigabyte model weights.

BUILD_DIR="${BUILD_DIR:-build}"
GATEWAY="${SAMOSA_COMPILED_GATEWAY:-./$BUILD_DIR/samosa-gateway}"
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/model_catalog_test.XXXXXX")
HOME_DIR="$TMP/home"
PORT=18988
PID=""

cleanup() {
  [ -z "$PID" ] || kill "$PID" 2>/dev/null || true
  [ -z "$PID" ] || wait "$PID" 2>/dev/null || true
  rm -rf "$TMP"
}

sha256_file() {
  target="${1:--}"
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$target" | awk '{print $1}'
  elif command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$target" | awk '{print $1}'
  else
    echo "FAIL: neither shasum nor sha256sum is installed" >&2
    return 127
  fi
}
trap cleanup EXIT HUP INT TERM



mkdir -p "$HOME_DIR"
printf '<!doctype html><title>Compiled Samosa</title>\n' >"$TMP/app.html"
printf 'png\n' >"$TMP/logo.png"

start_gateway() { # start_gateway <models-catalog-path>
  SAMOSA_HOME="$HOME_DIR" \
  SAMOSA_PORT="$PORT" \
  SAMOSA_BACKEND_PORT=$((PORT + 1)) \
  SAMOSA_APP_HTML="$TMP/app.html" \
  SAMOSA_APP_LOGO="$TMP/logo.png" \
  SAMOSA_MODELS_CATALOG="$1" \
    "$GATEWAY" >"$TMP/stdout.log" 2>"$TMP/stderr.log" &
  PID=$!
  i=0
  while [ "$i" -lt 50 ]; do
    curl -fsS "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1 && break
    sleep 0.05; i=$((i + 1))
  done
  TOKEN=$(cat "$HOME_DIR/run/ui-token")
}

stop_gateway() {
  kill "$PID" 2>/dev/null || true
  wait "$PID" 2>/dev/null || true
  PID=""
  rm -rf "$HOME_DIR"
  mkdir -p "$HOME_DIR"
}

# --- 1. The real bundled catalog, with nothing installed (normal CI state) ---
start_gateway "$ROOT/assets/models.json"

STATUS=$(curl -sS -o "$TMP/r1.json" -w '%{http_code}' "http://127.0.0.1:$PORT/v1/models/catalog")
[ "$STATUS" = "401" ] || { echo "FAIL: catalog without a token should be 401, got $STATUS"; cat "$TMP/r1.json"; exit 1; }

STATUS=$(curl -sS -o "$TMP/r2.json" -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/models/catalog")
[ "$STATUS" = "200" ] || { echo "FAIL: real catalog should serve 200, got $STATUS"; cat "$TMP/r2.json"; exit 1; }

python3 -c "
import json
d = json.load(open('$TMP/r2.json'))
assert d['schema_version'] == 1
assert d['runtime_abi'] == 'samosa-model-runtime-v1'
ids = sorted(m['id'] for m in d['models'])
assert ids == ['bonsai', 'ornith', 'qwen', 'voice-stt-whisper-base-en'], ids
by_id = {m['id']: m for m in d['models']}
# Nothing is installed in this sandboxed \$HOME_DIR -- every model must
# report not_installed, never a false 'ready'.
for mid, m in by_id.items():
    assert m['install_state'] == 'not_installed', (mid, m['install_state'])
    assert m['installed_bytes'] == 0, (mid, m['installed_bytes'])
    assert m['download_bytes'] > 0, (mid, m['download_bytes'])
    assert m['compatible'] is True, (mid, m['compatible'])  # reference machine is macOS arm64
# 'qwen' is the gateway's default selected backend even with nothing
# installed (matches /healthz's own \"backend\":\"qwen\" in this state,
# see test_zero_model_startup.sh) -- 'active' reflects current selection,
# not readiness, matching the existing /v1/backends semantics.
assert by_id['qwen']['active'] is True
assert by_id['bonsai']['active'] is False
assert by_id['ornith']['active'] is False
voice = by_id['voice-stt-whisper-base-en']
assert voice['family'] == 'voice'
assert voice['backend_kind'] == 'whisper_cpp'
assert voice['active'] is False
assert voice['install_state'] == 'not_installed'
assert voice['download_bytes'] == 147964211
assert voice['artifacts'][0]['sha256'] == 'a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002'
# Spot-check the real, independently-verified Qwen artifact facts (bytes
# and sha256 cross-checked against the actual hard-linked model and a
# prior real release manifest -- see docs/regressions/ui-chutni/t2.1-evidence.md).
qwen_artifacts = {a['name']: a for a in by_id['qwen']['artifacts']}
assert qwen_artifacts['experts.bin']['bytes'] == 20942159872
assert qwen_artifacts['experts.bin']['sha256'] == '00d64d44c39496e5ab5691f4cb27b67e27f3a10efd1f7c54024a9b43b130dbba'
assert qwen_artifacts['resident.safetensors']['bytes'] == 3015056192
assert by_id['qwen']['download_bytes'] == 20942159872 + 3015056192 + 1908179 + 3686 + 202 + 28142621
# Ornith has exactly one artifact with a real, HF-verified hash.
ornith_artifacts = {a['name']: a for a in by_id['ornith']['artifacts']}
assert ornith_artifacts['ornith-1.0-9b-Q4_K_M.gguf']['sha256'] == '5720d1f671b4996481274fffe01868c3c36e87c135cc8538471cc7bd6087b106'
# Bonsai's mmproj vision artifact is optional -- required text weights only.
bonsai_artifacts = {a['name']: a for a in by_id['bonsai']['artifacts']}
assert bonsai_artifacts['Bonsai-27B-Q1_0.gguf']['required'] is True
assert bonsai_artifacts['Bonsai-27B-mmproj-Q8_0.gguf']['required'] is False
# Live capability degradation: the mmproj file isn't installed in this
# sandboxed \$HOME_DIR, so 'image' must be dropped even though the catalog
# declares Bonsai image-capable -- 'text' must remain.
assert by_id['bonsai']['capabilities'] == ['text'], by_id['bonsai']['capabilities']
# Qwen's vision tower is built into its engine (no separate artifact) --
# its declared 'image' capability is never live-degraded.
assert by_id['qwen']['capabilities'] == ['text', 'image'], by_id['qwen']['capabilities']
print('real catalog: OK')
"

stop_gateway

# --- 2. Small deterministic fixture: one present+valid, one absent ---
# resolve_installed_artifact() (src/samosa_gateway.c) dispatches on the
# literal model id ("qwen"/"bonsai"/"ornith") to reuse the exact same
# per-backend path fields the rest of the gateway already trusts -- so
# these fixture catalog entries reuse those same real ids rather than
# inventing new ones the resolver wouldn't recognize.
FIXROOT="$TMP/fixmodels"
mkdir -p "$FIXROOT/qwen"
printf 'abcdefghij' >"$FIXROOT/qwen/experts.bin"   # 10 bytes, real content
FIXTURE_SHA=$(sha256_file "$FIXROOT/qwen/experts.bin")

cat >"$TMP/fixture_catalog.json" <<EOF
{
  "schema_version": 1,
  "catalog_revision": "test-fixture",
  "runtime_abi": "samosa-model-runtime-v1",
  "models": [
    {
      "id": "qwen",
      "version": "v1",
      "preferred_for_backend": true,
      "label": "Present",
      "description": "A fixture model whose one artifact is on disk.",
      "capabilities": ["text"],
      "backend_kind": "qwen_native",
      "supported_platforms": [{ "os": "macos", "architecture": "arm64" }],
      "required_runtime_abi": "samosa-model-runtime-v1",
      "minimum_ram_bytes": 0,
      "launch_profile_id": "qwen_native_default",
      "runtime_dependencies": [],
      "license": { "name": "test", "url": "https://huggingface.co/test" },
      "artifacts": [
        {
          "name": "experts.bin",
          "role": "weights",
          "required": true,
          "url": "",
          "install_path": "model/experts.bin",
          "file_mode": "0600",
          "bytes": 10,
          "sha256": "$FIXTURE_SHA"
        }
      ]
    },
    {
      "id": "ornith",
      "version": "v1",
      "preferred_for_backend": true,
      "label": "Missing",
      "description": "A fixture model whose artifact is never present and whose platform never matches this machine.",
      "capabilities": ["text"],
      "backend_kind": "llama_cpp",
      "supported_platforms": [{ "os": "windows", "architecture": "x86_64" }],
      "required_runtime_abi": "samosa-model-runtime-v1",
      "minimum_ram_bytes": 0,
      "launch_profile_id": "llama_cpp_default",
      "runtime_dependencies": [],
      "license": { "name": "test", "url": "https://huggingface.co/test" },
      "artifacts": [
        {
          "name": "ghost.gguf",
          "role": "weights",
          "required": true,
          "url": "https://huggingface.co/test/ghost.gguf",
          "install_path": "models/ghost.gguf",
          "file_mode": "0600",
          "bytes": 999,
          "sha256": "$(printf '%064d' 0)"
        }
      ]
    },
    {
      "id": "bonsai",
      "version": "v1",
      "preferred_for_backend": true,
      "label": "Weights present, shared dependency missing",
      "description": "Its own weights file is on disk, but the shared llama-server runtime dependency is not -- must not appear ready.",
      "capabilities": ["text"],
      "backend_kind": "llama_cpp",
      "supported_platforms": [{ "os": "macos", "architecture": "arm64" }],
      "required_runtime_abi": "samosa-model-runtime-v1",
      "minimum_ram_bytes": 0,
      "launch_profile_id": "llama_cpp_default",
      "runtime_dependencies": [
        { "package_id": "llama-server", "version": "prismml-fork", "source": "bundled" }
      ],
      "license": { "name": "test", "url": "https://huggingface.co/test" },
      "artifacts": [
        {
          "name": "Bonsai-27B-Q1_0.gguf",
          "role": "weights",
          "required": true,
          "url": "https://huggingface.co/test/bonsai.gguf",
          "install_path": "models/bonsai-27b-1bit/Bonsai-27B-Q1_0.gguf",
          "file_mode": "0600",
          "bytes": 6,
          "sha256": "$(printf 'bonsai' | sha256_file)"
        }
      ]
    }
  ]
}
EOF
mkdir -p "$FIXROOT/bonsai_weights_dir"
printf 'bonsai' >"$FIXROOT/bonsai_weights_dir/Bonsai-27B-Q1_0.gguf"

# qwen's one required artifact resolves via g->qwen_model joined with the
# artifact name "experts.bin" (resolve_installed_artifact()'s qwen case);
# ornith's SAMOSA_ORNITH_MODEL is deliberately left unset so it resolves to
# a default path that doesn't exist in this sandboxed $HOME_DIR; bonsai's
# weights file is present via SAMOSA_BONSAI_MODEL, but SAMOSA_BONSAI_SERVER
# is deliberately left unset (no llama-server binary exists at its default
# path here), exercising the shared-runtime-dependency gate on its own.
SAMOSA_QWEN_MODEL="$FIXROOT/qwen" \
SAMOSA_BONSAI_MODEL="$FIXROOT/bonsai_weights_dir/Bonsai-27B-Q1_0.gguf" \
SAMOSA_HOME="$HOME_DIR" \
SAMOSA_PORT="$PORT" \
SAMOSA_BACKEND_PORT=$((PORT + 1)) \
SAMOSA_APP_HTML="$TMP/app.html" \
SAMOSA_APP_LOGO="$TMP/logo.png" \
SAMOSA_MODELS_CATALOG="$TMP/fixture_catalog.json" \
  "$GATEWAY" >"$TMP/stdout.log" 2>"$TMP/stderr.log" &
PID=$!
i=0
while [ "$i" -lt 50 ]; do
  curl -fsS "http://127.0.0.1:$PORT/healthz" >/dev/null 2>&1 && break
  sleep 0.05; i=$((i + 1))
done
TOKEN=$(cat "$HOME_DIR/run/ui-token")

curl -sS -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/models/catalog" -o "$TMP/r3.json"
python3 -c "
import json
d = json.load(open('$TMP/r3.json'))
by_id = {m['id']: m for m in d['models']}
present = by_id['qwen']
assert present['install_state'] == 'ready', present['install_state']
assert present['installed_bytes'] == 10, present['installed_bytes']
assert present['compatible'] is True
missing = by_id['ornith']
# Wrong-platform entry: unavailable overrides not_installed, and a
# non-null compatibility_reason must be given.
assert missing['compatible'] is False
assert missing['install_state'] == 'unavailable', missing['install_state']
assert missing['compatibility_reason'], 'compatibility_reason must be non-empty when incompatible'
# Weights present, shared llama-server dependency absent: must still be
# not_installed, not a false 'ready' -- a shared dependency being missing
# must not let an incomplete model appear ready (T2.1 acceptance).
shared_dep_missing = by_id['bonsai']
assert shared_dep_missing['installed_bytes'] == 6, shared_dep_missing['installed_bytes']
assert shared_dep_missing['install_state'] == 'not_installed', shared_dep_missing['install_state']
print('fixture detection: OK')
"
stop_gateway

# --- 3. Malformed catalogs must be rejected (500 catalog_invalid), never
#        served or partially trusted ---
assert_catalog_rejected() { # assert_catalog_rejected <fixture-json-file> <case-name>
  fixture="$1"; name="$2"
  start_gateway "$fixture"
  STATUS=$(curl -sS -o "$TMP/bad.json" -w '%{http_code}' -H "X-Samosa-Token: $TOKEN" "http://127.0.0.1:$PORT/v1/models/catalog")
  [ "$STATUS" = "500" ] || { echo "FAIL ($name): expected 500, got $STATUS"; cat "$TMP/bad.json"; exit 1; }
  grep -q '"code":"catalog_invalid"' "$TMP/bad.json" || { echo "FAIL ($name): expected catalog_invalid code"; cat "$TMP/bad.json"; exit 1; }
  stop_gateway
}

base_entry() { # base_entry <id> <backend_kind> <install_path> <sha256> <required_runtime_abi>
  cat <<EOF
{ "id": "$1", "version": "v1", "preferred_for_backend": true, "label": "x",
  "description": "x", "capabilities": ["text"], "backend_kind": "$2",
  "supported_platforms": [{ "os": "macos", "architecture": "arm64" }],
  "required_runtime_abi": "$5", "minimum_ram_bytes": 0,
  "launch_profile_id": "x", "runtime_dependencies": [],
  "license": { "name": "x", "url": "https://huggingface.co/x" },
  "artifacts": [ { "name": "a", "role": "weights", "required": true,
    "url": "https://huggingface.co/x/a", "install_path": "$3",
    "file_mode": "0600", "bytes": 1, "sha256": "$4" } ] }
EOF
}

VALID_SHA=$(printf '%064d' 0)

printf '{ "schema_version": 1, "catalog_revision": "t", "runtime_abi": "samosa-model-runtime-v1", "models": [ %s, %s ] }' \
  "$(base_entry dup qwen_native model/a "$VALID_SHA" samosa-model-runtime-v1)" \
  "$(base_entry dup llama_cpp models/b "$VALID_SHA" samosa-model-runtime-v1)" \
  >"$TMP/bad_dup_id.json"
assert_catalog_rejected "$TMP/bad_dup_id.json" "duplicate model id"

printf '{ "schema_version": 1, "catalog_revision": "t", "runtime_abi": "samosa-model-runtime-v1", "models": [ %s, %s ] }' \
  "$(base_entry m1 qwen_native models/shared "$VALID_SHA" samosa-model-runtime-v1)" \
  "$(base_entry m2 llama_cpp models/shared "$VALID_SHA" samosa-model-runtime-v1)" \
  >"$TMP/bad_dup_path.json"
assert_catalog_rejected "$TMP/bad_dup_path.json" "duplicate install_path"

printf '{ "schema_version": 1, "catalog_revision": "t", "runtime_abi": "samosa-model-runtime-v1", "models": [ %s ] }' \
  "$(base_entry m1 qwen_native models/a not-a-valid-hash samosa-model-runtime-v1)" \
  >"$TMP/bad_hash.json"
assert_catalog_rejected "$TMP/bad_hash.json" "malformed sha256"

printf '{ "schema_version": 1, "catalog_revision": "t", "runtime_abi": "samosa-model-runtime-v1", "models": [ %s ] }' \
  "$(base_entry m1 qwen_native ../../etc/passwd "$VALID_SHA" samosa-model-runtime-v1)" \
  >"$TMP/bad_path.json"
assert_catalog_rejected "$TMP/bad_path.json" "unsafe install_path traversal"

printf '{ "schema_version": 1, "catalog_revision": "t", "runtime_abi": "samosa-model-runtime-v1", "models": [ %s ] }' \
  "$(base_entry m1 pytorch_native models/a "$VALID_SHA" samosa-model-runtime-v1)" \
  >"$TMP/bad_backend.json"
assert_catalog_rejected "$TMP/bad_backend.json" "unknown backend_kind"

printf '{ "schema_version": 1, "catalog_revision": "t", "runtime_abi": "samosa-model-runtime-v1", "models": [ %s ] }' \
  "$(base_entry m1 qwen_native models/a "$VALID_SHA" some-other-runtime-abi)" \
  >"$TMP/bad_abi.json"
assert_catalog_rejected "$TMP/bad_abi.json" "unsupported runtime_abi"

cat >"$TMP/bad_host.json" <<EOF
{ "schema_version": 1, "catalog_revision": "t", "runtime_abi": "samosa-model-runtime-v1",
  "models": [ { "id": "m1", "version": "v1", "preferred_for_backend": true, "label": "x",
  "description": "x", "capabilities": ["text"], "backend_kind": "qwen_native",
  "supported_platforms": [{ "os": "macos", "architecture": "arm64" }],
  "required_runtime_abi": "samosa-model-runtime-v1", "minimum_ram_bytes": 0,
  "launch_profile_id": "x", "runtime_dependencies": [],
  "license": { "name": "x", "url": "https://huggingface.co/x" },
  "artifacts": [ { "name": "a", "role": "weights", "required": true,
    "url": "https://evil.example.com/a", "install_path": "models/a",
    "file_mode": "0600", "bytes": 1, "sha256": "$VALID_SHA" } ] } ] }
EOF
assert_catalog_rejected "$TMP/bad_host.json" "untrusted artifact host"

echo "test_model_catalog.sh: PASS"

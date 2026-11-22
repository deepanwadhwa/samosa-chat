#!/bin/sh
set -eu

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

make samosa-gateway >/dev/null 2>&1 || true

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

node --input-type=module - "$TMP/r2.json" <<'JS'
import assert from 'node:assert/strict';
import fs from 'node:fs';

const catalog = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
assert.equal(catalog.schema_version, 1);
assert.equal(catalog.runtime_abi, 'samosa-model-runtime-v1');
assert.deepEqual(catalog.models.map(model => model.id).sort(), [
  'bonsai', 'maple', 'molmo2-4b-mlx-q4-v1', 'ornith', 'qwen', 'visionpsy-nano-460m-mlx-bf16',
  'voice-stt-whisper-base-en', 'voice-stt-whisper-tiny-en', 'voice-tts-browser',
  'voice-tts-kitten-nano', 'voice-tts-kokoro', 'voice-tts-moss-nano', 'voice-tts-pocket'
]);
const models = Object.fromEntries(catalog.models.map(model => [model.id, model]));
const localTts = ['pocket_tts', 'kokoro_tts', 'moss_tts', 'kitten_tts'];
for (const model of catalog.models) {
  assert.equal(model.compatible, true, model.id);
  if (model.id === 'voice-tts-browser') {
    assert.equal(model.install_state, 'ready');
    assert.equal(model.download_bytes, 0);
    continue;
  }
  assert.equal(model.install_state, 'not_installed', model.id);
  assert.equal(model.installed_bytes, 0, model.id);
  if (model.provisioning !== 'native_pack' && !localTts.includes(model.backend_kind))
    assert.ok(model.download_bytes > 0, model.id);
}

// The default selection remains Qwen while the gateway waits for a model.
assert.equal(models.qwen.active, true);
assert.equal(models.bonsai.active, false);
assert.equal(models.maple.active, false);
assert.equal(models.ornith.active, false);

const visionpsy = models['visionpsy-nano-460m-mlx-bf16'];
assert.equal(visionpsy.family, 'vision');
assert.equal(visionpsy.category, 'vision');
assert.equal(visionpsy.role, 'auxiliary');
assert.equal(visionpsy.backend_kind, 'mlx_vision_native');
assert.equal(visionpsy.active, false);
assert.equal(visionpsy.routing, 'automatic');
assert.equal(visionpsy.load_policy, 'on_demand_per_turn');
assert.equal(visionpsy.download_bytes, 1014772920 + 7364 + 2622 + 142 + 3535344 + 4207 + 196);

const molmo = models['molmo2-4b-mlx-q4-v1'];
assert.equal(molmo.family, 'chat');
assert.equal(molmo.category, 'chat');
assert.equal(molmo.role, 'primary');
assert.equal(molmo.provisioning, 'native_pack');
assert.equal(molmo.upstream_revision, '042abfa7a38879a376cec03d949eff0aefaa0600');
assert.equal(molmo.routing, 'direct_or_automatic');
assert.equal(molmo.input_policy, 'visual_required');
assert.deepEqual(molmo.output_modalities, ['text', 'grounding_coordinates']);
assert.equal(molmo.download_bytes, 0);
assert.equal(molmo.install_state, 'not_installed');
assert.ok(molmo.capabilities.includes('video_understanding'));

const voice = models['voice-stt-whisper-base-en'];
assert.equal(voice.family, 'voice');
assert.equal(voice.backend_kind, 'whisper_cpp');
assert.equal(voice.active, false);
assert.equal(voice.install_state, 'not_installed');
assert.equal(voice.download_bytes, 147964211);
assert.equal(voice.artifacts[0].sha256, 'a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002');
const tiny = models['voice-stt-whisper-tiny-en'];
assert.equal(tiny.backend_kind, 'whisper_cpp');
assert.equal(tiny.artifacts[0].bytes, 77704715);
assert.equal(tiny.artifacts[0].sha256, '921e4cf8686fdd993dcd081a5da5b6c365bfde1162e72b08d75ac75289920b1f');
for (const id of ['voice-tts-pocket', 'voice-tts-moss-nano', 'voice-tts-kitten-nano']) {
  assert.ok(models[id].pros?.length, id);
  assert.ok(models[id].cons?.length, id);
}
assert.equal(models['voice-tts-kokoro'].voice_role, 'tts');
assert.equal(models['voice-tts-moss-nano'].backend_kind, 'moss_tts');
assert.equal(models['voice-tts-kitten-nano'].backend_kind, 'kitten_tts');

// The Qwen sizes and hash were cross-checked against the release model in T2.1.
const byName = model => Object.fromEntries(model.artifacts.map(artifact => [artifact.name, artifact]));
const qwen = byName(models.qwen);
assert.equal(qwen['experts.bin'].bytes, 20942159872);
assert.equal(qwen['experts.bin'].sha256, '00d64d44c39496e5ab5691f4cb27b67e27f3a10efd1f7c54024a9b43b130dbba');
assert.equal(qwen['resident.safetensors'].bytes, 3015056192);
assert.equal(models.qwen.download_bytes, 20942159872 + 3015056192 + 1908179 + 3686 + 202 + 28142621);

// Maple's SSD-streaming pack stays in the persistent models directory.
const maple = byName(models.maple);
assert.deepEqual(Object.keys(maple).sort(), [
  'maple-experts.bin', 'maple-resident.safetensors', 'maple-manifest.json',
  'config.json', 'tokenizer.json', 'tokenizer_config.json', 'special_tokens_map.json',
  'added_tokens.json', 'merges.txt', 'vocab.json', 'chat_template.jinja'
].sort());
for (const artifact of Object.values(maple)) {
  assert.ok(artifact.install_path.startsWith('models/maple/'), artifact.name);
  assert.ok(artifact.url.includes('/deepanwa/Samosa-Chat-Maple-2bit-SSD/resolve/'), artifact.name);
}
assert.ok(models.maple.download_bytes < 6_000_000_000);
const ornith = byName(models.ornith);
assert.equal(ornith['ornith-1.0-9b-Q4_K_M.gguf'].sha256, '5720d1f671b4996481274fffe01868c3c36e87c135cc8538471cc7bd6087b106');

// Bonsai's image capability requires the optional projector. Qwen includes its own vision tower.
const bonsai = byName(models.bonsai);
assert.equal(bonsai['Bonsai-27B-Q1_0.gguf'].required, true);
assert.equal(bonsai['Bonsai-27B-mmproj-Q8_0.gguf'].required, false);
assert.deepEqual(models.bonsai.capabilities, ['text']);
assert.deepEqual(models.qwen.capabilities, ['text', 'image']);
console.log('real catalog: OK');
JS

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
    },
    {
      "id": "maple",
      "version": "v1",
      "preferred_for_backend": true,
      "label": "Packed Maple present",
      "description": "Fixture proving the bundled Maple runtime dependency is recognized.",
      "capabilities": ["text"],
      "backend_kind": "mlx_native",
      "supported_platforms": [{ "os": "macos", "architecture": "arm64" }],
      "required_runtime_abi": "samosa-model-runtime-v1",
      "minimum_ram_bytes": 0,
      "launch_profile_id": "mlx_native_default",
      "runtime_dependencies": [
        { "package_id": "samosa-maple", "version": "v1", "source": "bundled" }
      ],
      "license": { "name": "test", "url": "https://huggingface.co/test" },
      "artifacts": [
        {
          "name": "maple-manifest.json",
          "role": "configuration",
          "required": true,
          "url": "https://huggingface.co/test/maple-manifest.json",
          "install_path": "models/maple/maple-manifest.json",
          "file_mode": "0600",
          "bytes": 3,
          "sha256": "$(printf '{}\n' | sha256_file)"
        }
      ]
    }
  ]
}
EOF
mkdir -p "$FIXROOT/bonsai_weights_dir"
printf 'bonsai' >"$FIXROOT/bonsai_weights_dir/Bonsai-27B-Q1_0.gguf"
mkdir -p "$FIXROOT/maple"
printf '{}\n' >"$FIXROOT/maple/maple-manifest.json"
printf '#!/bin/sh\nexit 0\n' >"$FIXROOT/samosa-maple"
chmod +x "$FIXROOT/samosa-maple"

# qwen's one required artifact resolves via g->qwen_model joined with the
# artifact name "experts.bin" (resolve_installed_artifact()'s qwen case);
# ornith's SAMOSA_ORNITH_MODEL is deliberately left unset so it resolves to
# a default path that doesn't exist in this sandboxed $HOME_DIR; bonsai's
# weights file is present via SAMOSA_BONSAI_MODEL, but SAMOSA_BONSAI_SERVER
# is deliberately left unset (no llama-server binary exists at its default
# path here), exercising the shared-runtime-dependency gate on its own.
SAMOSA_QWEN_MODEL="$FIXROOT/qwen" \
SAMOSA_BONSAI_MODEL="$FIXROOT/bonsai_weights_dir/Bonsai-27B-Q1_0.gguf" \
SAMOSA_MAPLE_MODEL="$FIXROOT/maple" \
SAMOSA_MAPLE_ENGINE="$FIXROOT/samosa-maple" \
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
node --input-type=module - "$TMP/r3.json" <<'JS'
import assert from 'node:assert/strict';
import fs from 'node:fs';

const catalog = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
const models = Object.fromEntries(catalog.models.map(model => [model.id, model]));
assert.equal(models.qwen.install_state, 'ready');
assert.equal(models.qwen.installed_bytes, 10);
assert.equal(models.qwen.compatible, true);
// A wrong-platform entry reports its incompatibility before install state.
assert.equal(models.ornith.compatible, false);
assert.equal(models.ornith.install_state, 'unavailable');
assert.ok(models.ornith.compatibility_reason, 'compatibility_reason must be non-empty when incompatible');
// Bonsai needs llama-server as well as weights before it can run.
assert.equal(models.bonsai.installed_bytes, 6);
assert.equal(models.bonsai.install_state, 'not_installed');
assert.equal(models.maple.installed_bytes, 3);
assert.equal(models.maple.install_state, 'ready');
console.log('fixture detection: OK');
JS
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

#!/bin/sh
# Install the current source build as a local development release without
# copying the large group-32 model. Model payloads are hard-linked on APFS.

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
MODEL_ROOT=${SAMOSA_MODELS_DIR:-"$(dirname "$ROOT")/samosa-models"}
SNAPSHOT=${SAMOSA_SNAPSHOT:-"$MODEL_ROOT/qwen36_group32_i8"}
TOKENIZER=${SAMOSA_TOKENIZER:-"$MODEL_ROOT/tokenizer_qwen36.json"}
HOME_DIR=${SAMOSA_HOME:-"$HOME/.samosa"}
ENGINE="$ROOT/qwen36b"

for path in "$ENGINE" "$ROOT/assets/app.html" "$ROOT/assets/samosa-chat.png" \
  "$ROOT/dist/samosa" "$ROOT/tools/samosa_gateway.py" \
  "$SNAPSHOT/experts.bin" "$SNAPSHOT/resident.safetensors" \
  "$SNAPSHOT/manifest.json" "$SNAPSHOT/config.json" \
  "$SNAPSHOT/generation_config.json" "$TOKENIZER"; do
  [ -f "$path" ] || { echo "missing local development input: $path" >&2; exit 1; }
done

release_hash=$(shasum -a 256 "$ENGINE" "$ROOT/assets/app.html" "$ROOT/dist/samosa" \
  "$ROOT/tools/samosa_gateway.py" |
  shasum -a 256 | awk '{print substr($1,1,12)}')
release_id="dev-$release_hash"
stage="$HOME_DIR/releases/.${release_id}.partial.$$"
final="$HOME_DIR/releases/$release_id"
trap 'rm -rf "$stage"' EXIT HUP INT TERM
mkdir -p "$stage/bin" "$stage/model" "$HOME_DIR/releases" "$HOME_DIR/bin"

for name in experts.bin resident.safetensors manifest.json config.json generation_config.json; do
  ln "$SNAPSHOT/$name" "$stage/model/$name" || {
    echo "hard-link failed for $name; refusing to duplicate the model" >&2
    exit 1
  }
done
ln "$TOKENIZER" "$stage/tokenizer_qwen36.json" || {
  echo "hard-link failed for tokenizer; refusing an implicit copy" >&2
  exit 1
}
cp "$ENGINE" "$stage/bin/qwen36b"
cp "$ROOT/dist/samosa" "$stage/bin/samosa"
cp "$ROOT/tools/samosa_gateway.py" "$stage/bin/samosa-gateway"
cp "$ROOT/assets/app.html" "$stage/app.html"
cp "$ROOT/assets/samosa-chat.png" "$stage/samosa-chat.png"
chmod +x "$stage/bin/qwen36b" "$stage/bin/samosa" "$stage/bin/samosa-gateway"

if [ ! -d "$final" ]; then mv "$stage" "$final"; else rm -rf "$stage"; fi
rm -f "$HOME_DIR/.current.next"
ln -s "releases/$release_id" "$HOME_DIR/.current.next"
mv -fh "$HOME_DIR/.current.next" "$HOME_DIR/current"

cat >"$HOME_DIR/bin/samosa" <<'EOF'
#!/bin/sh
set -eu
HOME_DIR="${SAMOSA_HOME:-$HOME/.samosa}"
exec "$HOME_DIR/current/bin/samosa" "$@"
EOF
chmod +x "$HOME_DIR/bin/samosa"
trap - EXIT HUP INT TERM

echo "Installed local development release $release_id"
echo "Launcher: $HOME_DIR/bin/samosa"
echo "Model files were hard-linked, not copied."

# Unlike dist/install.sh, this script never edits your shell rc — a dev install
# should not mutate your profile behind your back. So say plainly whether the
# launcher is reachable, instead of leaving you to find out via
# "command not found".
case ":$PATH:" in
  *":$HOME_DIR/bin:"*)
    echo "PATH: ok — 'samosa' is runnable in this shell."
    ;;
  *)
    echo
    echo "NOTE: $HOME_DIR/bin is not on your PATH, so 'samosa' will not be found."
    echo "      For this shell:   export PATH=\"\$HOME/.samosa/bin:\$PATH\""
    echo "      To make it stick: echo 'export PATH=\"\$HOME/.samosa/bin:\$PATH\"' >> ~/.zshrc"
    echo "      Or run it directly: $HOME_DIR/bin/samosa \"how are you\""
    ;;
esac

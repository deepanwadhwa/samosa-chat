#!/bin/sh
# Install Samosa's opt-in native Kokoro TTS runtime. No Python or pip.
set -eu

HOME_DIR=${SAMOSA_HOME:-"$HOME/.samosa"}
VOICE_DIR="$HOME_DIR/voice/kokoro"
RUNTIME_DIR="$VOICE_DIR/runtime"
MODEL_DIR="$VOICE_DIR/model"
READY="$VOICE_DIR/ready"
VERSION=1.13.4
MODEL_NAME=kokoro-int8-en-v0_19
MODEL_URL="https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/$MODEL_NAME.tar.bz2"
MODEL_SHA256=c9f0dd393615805b0bab050c340834d5e684e732aec91c0e860cd30e982c08bd

case "$(uname -s)-$(uname -m)" in
  Darwin-arm64)
    RUNTIME_NAME="sherpa-onnx-v$VERSION-osx-arm64-shared-lib"
    RUNTIME_SHA256=995d38d323eef0bfbfe7432dcceffda91bbd95525a15fa64fed517ed368378b9
    ;;
  Darwin-x86_64)
    RUNTIME_NAME="sherpa-onnx-v$VERSION-osx-x64-shared-lib"
    RUNTIME_SHA256=24d37d744b9f4b6b6bff618ede6cede527d7c0073fcddeb554b5d13242a4544b
    ;;
  *) echo "Kokoro's native voice is currently available for Intel and Apple Silicon Macs." >&2; exit 1 ;;
esac
RUNTIME_URL="https://github.com/k2-fsa/sherpa-onnx/releases/download/v$VERSION/$RUNTIME_NAME.tar.bz2"

[ -f "$READY" ] && [ -f "$RUNTIME_DIR/lib/libsherpa-onnx-c-api.dylib" ] && \
  [ -f "$MODEL_DIR/model.int8.onnx" ] && [ -f "$MODEL_DIR/voices.bin" ] && exit 0
command -v curl >/dev/null 2>&1 || { echo "curl is required to download the native voice." >&2; exit 1; }
command -v tar >/dev/null 2>&1 || { echo "tar is required to unpack the native voice." >&2; exit 1; }
command -v shasum >/dev/null 2>&1 || { echo "shasum is required to verify the native voice download." >&2; exit 1; }
command -v codesign >/dev/null 2>&1 || { echo "macOS codesign is required to prepare the native voice runtime." >&2; exit 1; }

mkdir -p "$VOICE_DIR"
WORK=$(mktemp -d "$VOICE_DIR/.download.XXXXXX")
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT HUP INT TERM

download_verified() {
  url=$1 expected=$2 output=$3
  curl -fL --retry 2 --connect-timeout 20 --max-time 900 "$url" -o "$output"
  actual=$(shasum -a 256 "$output" | awk '{print $1}')
  [ "$actual" = "$expected" ] || { echo "download failed its pinned checksum" >&2; exit 1; }
}

download_verified "$RUNTIME_URL" "$RUNTIME_SHA256" "$WORK/runtime.tar.bz2"
download_verified "$MODEL_URL" "$MODEL_SHA256" "$WORK/model.tar.bz2"
tar -xjf "$WORK/runtime.tar.bz2" -C "$WORK"
tar -xjf "$WORK/model.tar.bz2" -C "$WORK"
[ -f "$WORK/$RUNTIME_NAME/lib/libsherpa-onnx-c-api.dylib" ] || { echo "native voice runtime archive is incomplete" >&2; exit 1; }
[ -f "$WORK/$MODEL_NAME/model.int8.onnx" ] && [ -f "$WORK/$MODEL_NAME/voices.bin" ] && \
  [ -f "$WORK/$MODEL_NAME/tokens.txt" ] && [ -d "$WORK/$MODEL_NAME/espeak-ng-data" ] || { echo "Kokoro model archive is incomplete" >&2; exit 1; }
# Recent macOS releases validate every executable page when dlopen() maps a
# downloaded dylib. The upstream archive is checksum-verified above; signing
# the extracted local copy afterwards gives it a signature this Mac can trust.
codesign --force --sign - --timestamp=none "$WORK/$RUNTIME_NAME/lib"/*.dylib || {
  echo "macOS could not sign the verified native voice runtime." >&2; exit 1;
}

rm -rf "$VOICE_DIR/runtime.next" "$VOICE_DIR/model.next"
mv "$WORK/$RUNTIME_NAME" "$VOICE_DIR/runtime.next"
mv "$WORK/$MODEL_NAME" "$VOICE_DIR/model.next"
rm -rf "$RUNTIME_DIR" "$MODEL_DIR"
mv "$VOICE_DIR/runtime.next" "$RUNTIME_DIR"
mv "$VOICE_DIR/model.next" "$MODEL_DIR"
printf 'sherpa-onnx-%s / %s\n' "$VERSION" "$MODEL_NAME" >"$READY.tmp"
mv "$READY.tmp" "$READY"

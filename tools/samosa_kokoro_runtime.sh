#!/bin/sh
# Install Samosa's low-latency Pocket TTS runtime. The shipped filename is
# retained for release compatibility with older installers. No Python or pip.
set -eu

HOME_DIR=${SAMOSA_HOME:-"$HOME/.samosa"}
VOICE_DIR="$HOME_DIR/voice/pocket"
RUNTIME_DIR="$VOICE_DIR/runtime"
MODEL_DIR="$VOICE_DIR/model"
READY="$VOICE_DIR/ready"
VERSION=1.13.4
MODEL_NAME=sherpa-onnx-pocket-tts-int8-2026-01-26
MODEL_URL="https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/$MODEL_NAME.tar.bz2"
MODEL_SHA256=2f3b88823cbbb9bf0b2477ec8ae7b3fec417b3a87b6bb5f256dba66f2ad967cb
VOICE_REV=323332d33f997de8394f24a193e1a76df720e01a
CARO_URL="https://huggingface.co/kyutai/tts-voices/resolve/$VOICE_REV/voice-zero/caro_davy.wav"
CARO_SHA256=40c692c005a0268a7a5b6ebae348077d3dca6a86eb6b12bd36e343bbcd71b5f6
STUART_URL="https://huggingface.co/kyutai/tts-voices/resolve/$VOICE_REV/voice-zero/stuart_bell.wav"
STUART_SHA256=00c7baeb2fb7a8c1c6198e045b5e853a7ccc04002a51a09b4be3dd7c96994f73
LEGACY_RUNTIME="$HOME_DIR/voice/kokoro/runtime"

case "$(uname -s)-$(uname -m)" in
  Darwin-arm64)
    RUNTIME_NAME="sherpa-onnx-v$VERSION-osx-arm64-shared-lib"
    RUNTIME_SHA256=995d38d323eef0bfbfe7432dcceffda91bbd95525a15fa64fed517ed368378b9
    ;;
  Darwin-x86_64)
    RUNTIME_NAME="sherpa-onnx-v$VERSION-osx-x64-shared-lib"
    RUNTIME_SHA256=24d37d744b9f4b6b6bff618ede6cede527d7c0073fcddeb554b5d13242a4544b
    ;;
  *) echo "Pocket TTS is currently available for Intel and Apple Silicon Macs." >&2; exit 1 ;;
esac
RUNTIME_URL="https://github.com/k2-fsa/sherpa-onnx/releases/download/v$VERSION/$RUNTIME_NAME.tar.bz2"

[ -f "$READY" ] && [ -f "$RUNTIME_DIR/lib/libsherpa-onnx-c-api.dylib" ] && \
  [ -f "$MODEL_DIR/lm_flow.int8.onnx" ] && [ -f "$MODEL_DIR/lm_main.int8.onnx" ] && \
  [ -f "$MODEL_DIR/encoder.onnx" ] && [ -f "$MODEL_DIR/decoder.int8.onnx" ] && \
  [ -f "$MODEL_DIR/text_conditioner.onnx" ] && [ -f "$MODEL_DIR/vocab.json" ] && \
  [ -f "$MODEL_DIR/token_scores.json" ] && [ -f "$MODEL_DIR/voices/caro_davy.wav" ] && \
  [ -f "$MODEL_DIR/voices/stuart_bell.wav" ] && exit 0
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

download_verified "$MODEL_URL" "$MODEL_SHA256" "$WORK/model.tar.bz2"
download_verified "$CARO_URL" "$CARO_SHA256" "$WORK/caro_davy.wav"
download_verified "$STUART_URL" "$STUART_SHA256" "$WORK/stuart_bell.wav"
tar -xjf "$WORK/model.tar.bz2" -C "$WORK"
[ -f "$WORK/$MODEL_NAME/lm_flow.int8.onnx" ] && \
  [ -f "$WORK/$MODEL_NAME/lm_main.int8.onnx" ] && \
  [ -f "$WORK/$MODEL_NAME/encoder.onnx" ] && \
  [ -f "$WORK/$MODEL_NAME/decoder.int8.onnx" ] && \
  [ -f "$WORK/$MODEL_NAME/text_conditioner.onnx" ] && \
  [ -f "$WORK/$MODEL_NAME/vocab.json" ] && \
  [ -f "$WORK/$MODEL_NAME/token_scores.json" ] || {
    echo "Pocket TTS model archive is incomplete" >&2; exit 1;
  }

runtime_source=
if [ -f "$LEGACY_RUNTIME/lib/libsherpa-onnx-c-api.dylib" ]; then
  runtime_source=$LEGACY_RUNTIME
else
  download_verified "$RUNTIME_URL" "$RUNTIME_SHA256" "$WORK/runtime.tar.bz2"
  tar -xjf "$WORK/runtime.tar.bz2" -C "$WORK"
  [ -f "$WORK/$RUNTIME_NAME/lib/libsherpa-onnx-c-api.dylib" ] || {
    echo "native voice runtime archive is incomplete" >&2; exit 1;
  }
  codesign --force --sign - --timestamp=none "$WORK/$RUNTIME_NAME/lib"/*.dylib || {
    echo "macOS could not sign the verified native voice runtime." >&2; exit 1;
  }
  runtime_source="$WORK/$RUNTIME_NAME"
fi

mkdir -p "$WORK/$MODEL_NAME/voices"
mv "$WORK/caro_davy.wav" "$WORK/$MODEL_NAME/voices/caro_davy.wav"
mv "$WORK/stuart_bell.wav" "$WORK/$MODEL_NAME/voices/stuart_bell.wav"
rm -rf "$VOICE_DIR/runtime.next" "$VOICE_DIR/model.next"
if [ "$runtime_source" = "$LEGACY_RUNTIME" ]; then
  ln -s ../kokoro/runtime "$VOICE_DIR/runtime.next"
else
  mv "$runtime_source" "$VOICE_DIR/runtime.next"
fi
mv "$WORK/$MODEL_NAME" "$VOICE_DIR/model.next"
rm -rf "$RUNTIME_DIR" "$MODEL_DIR"
mv "$VOICE_DIR/runtime.next" "$RUNTIME_DIR"
mv "$VOICE_DIR/model.next" "$MODEL_DIR"
printf 'sherpa-onnx-%s / %s / voice-zero CC0\n' "$VERSION" "$MODEL_NAME" >"$READY.tmp"
mv "$READY.tmp" "$READY"

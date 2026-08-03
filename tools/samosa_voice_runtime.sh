#!/bin/sh
# Build the pinned local Whisper.cpp command-line runtime used by Samosa voice.
# This is intentionally a separate, explicit bootstrap from the weight
# download: the source is checksum-verified, the result lives under Samosa's
# own home, and neither the model nor audio ever leaves the computer at use
# time. The gateway serializes invocations of this script.

set -eu

HOME_DIR=${SAMOSA_HOME:-"$HOME/.samosa"}
RUNTIME_DIR="$HOME_DIR/voice/runtime"
CLI="$RUNTIME_DIR/whisper-cli"
COMMIT="f049fff95a089aa9969deb009cdd4892b3e74916"
SOURCE_URL="https://api.github.com/repos/ggml-org/whisper.cpp/tarball/$COMMIT"
SOURCE_SHA256="d8cd961352377b1cc612224016a9ebdfe0ae508dc2b2f9ef514b341d672e3fdc"

sha256_file() {
  if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$1" | awk '{print $1}';
  elif command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}';
  else return 1; fi
}

[ -x "$CLI" ] && exit 0
command -v curl >/dev/null 2>&1 || { echo "curl is required to prepare local speech recognition." >&2; exit 1; }
command -v tar >/dev/null 2>&1 || { echo "tar is required to prepare local speech recognition." >&2; exit 1; }

# launchd services receive a deliberately small PATH. Homebrew's CMake lives
# outside it on a normal Apple Silicon install, even though it is installed.
# Honour an explicit override first, then resolve the two standard locations.
if [ -n "${SAMOSA_CMAKE:-}" ] && [ -x "$SAMOSA_CMAKE" ]; then
  CMAKE="$SAMOSA_CMAKE"
elif command -v cmake >/dev/null 2>&1; then
  CMAKE=$(command -v cmake)
elif [ -x /opt/homebrew/bin/cmake ]; then
  CMAKE=/opt/homebrew/bin/cmake
elif [ -x /usr/local/bin/cmake ]; then
  CMAKE=/usr/local/bin/cmake
else
  echo "CMake is required to set up local voice recognition. Install it with: brew install cmake" >&2
  exit 1
fi

mkdir -p "$RUNTIME_DIR"
TMP=$(mktemp -d "$RUNTIME_DIR/.whisper-build.XXXXXX")
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

curl -fsSL --retry 3 --retry-delay 2 "$SOURCE_URL" -o "$TMP/source.tar.gz"
[ "$(sha256_file "$TMP/source.tar.gz")" = "$SOURCE_SHA256" ] || {
  echo "The Whisper.cpp source archive did not match its pinned checksum." >&2
  exit 1
}

mkdir "$TMP/source"
tar -xzf "$TMP/source.tar.gz" -C "$TMP/source" --strip-components 1
"$CMAKE" -S "$TMP/source" -B "$TMP/build" \
  -DBUILD_SHARED_LIBS=OFF -DWHISPER_BUILD_EXAMPLES=ON \
  -DWHISPER_BUILD_TESTS=OFF -DWHISPER_BUILD_SERVER=OFF
"$CMAKE" --build "$TMP/build" --target whisper-cli --parallel 2
[ -x "$TMP/build/bin/whisper-cli" ] || { echo "Whisper.cpp did not produce whisper-cli." >&2; exit 1; }

cp "$TMP/build/bin/whisper-cli" "$RUNTIME_DIR/.whisper-cli.new"
chmod 700 "$RUNTIME_DIR/.whisper-cli.new"
mv -f "$RUNTIME_DIR/.whisper-cli.new" "$CLI"

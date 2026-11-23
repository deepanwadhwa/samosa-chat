#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
exec "$ROOT/components/samosa-inference/tools/samosa_voice_runtime.sh" "$@"

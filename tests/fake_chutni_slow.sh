#!/bin/sh
set -eu

fail() {
  echo "$(basename "$0"): FAIL: $1" >&2
  exit 1
}

REAL=${REAL_CHUTNI_MCP:?REAL_CHUTNI_MCP is required}

if [ "${1:-}" = "--call" ] && [ "${2:-}" = "chutni_folder_activate" ]; then
  i=0
  while [ "$i" -lt 80 ]; do
    i=$((i + 1))
    printf '{"type":"scan_progress","files_seen":%s,"sources_indexed":%s,"unchanged":0,"text_artifacts":%s,"metadata_artifacts":0,"skipped":0,"errors":0,"current_path":"/fixture/file-%s.txt"}\n' \
      "$i" "$i" "$i" "$i" >&2
    sleep 0.05
  done
fi

exec "$REAL" "$@"

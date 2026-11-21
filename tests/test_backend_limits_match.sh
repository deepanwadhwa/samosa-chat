#!/bin/sh
set -eu
# tests/test_backend_limits.c mirrors backend_limits() from the gateway so the
# tier table can be checked for machines this repo cannot run on. A mirror that
# drifts is worse than no test, so the tier constants are compared directly.
GW=${SAMOSA_GATEWAY_SRC:-src/samosa_gateway.c}
T=${SAMOSA_LIMITS_TEST:-tests/test_backend_limits.c}
for f in "$GW" "$T"; do [ -f "$f" ] || { echo "test_backend_limits_match.sh: $f not found" >&2; exit 1; }; done
node "$(dirname "$0")/backend_limits_match.mjs" "$GW" "$T"

#!/bin/sh
set -eu
printf '%s\n' "$$" >"$SAMOSA_TEST_CHUTNI_PID_FILE"
exec "$SAMOSA_REAL_CHUTNI_SERVICE" "$@"

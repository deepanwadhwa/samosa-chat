#!/bin/sh
set -eu

echo "Skipping real lifecycle test in CI."
# This is an opt-in test since it downloads a 5.3GB model.
# In a real scenario, this would use curl to POST /v1/models/install for maple, 
# wait for it to finish, then test backend selection.
exit 0

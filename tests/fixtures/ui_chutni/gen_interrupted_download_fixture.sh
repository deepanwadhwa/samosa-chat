#!/bin/sh
set -eu

# Shared "interrupted model download" fixture (docs/TASKS_UI_CHUTNI.md T0.1,
# reused by T1.2's active_install_job_id repair test and T2.2's resume test).
# Builds the on-disk partial-artifact layout from §5.3 --
#   ~/.samosa/models/<model-id>/.partial/<job-id>/
# -- with a small deterministic partial byte range, alongside the matching
# job-registry snapshot at tests/fixtures/ui_chutni/contracts/interrupted_install_job.json
# (job_id f1a2b3c4d5e6f1a2b3c4d5e6f1a2b3c4, 2048 of 8192 bytes retained).
#
# Usage: gen_interrupted_download_fixture.sh HOME_DIR

home="${1:?usage: gen_interrupted_download_fixture.sh HOME_DIR}"
job_id="f1a2b3c4d5e6f1a2b3c4d5e6f1a2b3c4"
partial_dir="$home/models/qwen/.partial/$job_id"

mkdir -p "$partial_dir"
python3 -c "
data = bytes((i * 17 + 3) % 256 for i in range(2048))
open('$partial_dir/weights.partial', 'wb').write(data)
"

cat >"$home/models/registry.json" <<EOF
{
  "schema_version": 1,
  "models": []
}
EOF

echo "gen_interrupted_download_fixture.sh: placed partial job $job_id (2048/8192 bytes) under $home/models/qwen"

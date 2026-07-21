# Package install serve dry run — 2026-07-21

Scope: T10 package -> install -> serve dry run for gateway Jobs packaging.
The live serve check used Ornith, not the 24 GB Qwen model.

Temporary workspace:

```text
/tmp/samosa-t10-dryrun.6TXFVr
```

## Package

Command:

```sh
python3 tools/package_hf.py \
  --out /tmp/samosa-t10-dryrun.6TXFVr/remote \
  --snapshot /tmp/samosa-t10-dryrun.6TXFVr/snapshot \
  --tokenizer /tmp/samosa-t10-dryrun.6TXFVr/tokenizer.json \
  --repo-id test/samosa \
  --gateway
```

Manifest checks:

```text
c3c31d2debc57e3b35401c915945131cbf0882789c9d2092f1fec714c34e1561  26563  engine/samosa_fs.c
cee26e2f656bb706b2fc6bea1a4ef9867d0df719216ac5855a43a304f1a2b302  56326  samosa-gateway
```

## Install

Command:

```sh
SAMOSA_INSTALL_TEST=1 \
SAMOSA_SKIP_PATH_SETUP=1 \
SAMOSA_MIN_FREE_AFTER_GB=0 \
SAMOSA_BASE_URL=file:///tmp/samosa-t10-dryrun.6TXFVr/remote \
SAMOSA_HOME=/tmp/samosa-t10-dryrun.6TXFVr/home \
sh dist/install.sh
```

Installed files:

```text
/tmp/samosa-t10-dryrun.6TXFVr/home/current/bin/samosa-fs
/tmp/samosa-t10-dryrun.6TXFVr/home/current/bin/samosa-gateway
/tmp/samosa-t10-dryrun.6TXFVr/home/current/bin/jobs_fs.py
```

Installed `jobs_fs.py` -> installed `samosa-fs` check:

```text
error= None
{"by_type": {"text/plain": {"bytes": 5, "count": 1}}, "ok": true, "skipped": [], "skipped_count": 0, "total": 1}
```

## Serve

The installed release was launched with `model-backend` set to `ornith` and
with `SAMOSA_ORNITH_MODEL` / `SAMOSA_BONSAI_SERVER` pointing at the local Ornith
install.

Health:

```json
{"gateway":true,"backend":"ornith","label":"Ornith 9B","model":"ornith-1.0-9b","supports_images":false,"ready":true,"loading":false,"generating":false,"pid":9269}
```

Installed Jobs route check:

```text
POST /v1/jobs/run {"goal":"how many files are here?","folder":"/tmp/samosa-installed-job.tdm0O0","mode":"confirm"}

seq1 decode_intent
seq2 intent kind=report
seq3 counting total=1 skipped=0 by_type={text/plain:1}
seq4 report total=1 by_type={text/plain:1}
seq5 done "1 files: 1 text/plain."
```

Result: passed. The gateway package includes `samosa-fs`, the installer compiles
and stages it, the installed gateway can find it, and the installed Jobs route
uses it successfully while served with Ornith.

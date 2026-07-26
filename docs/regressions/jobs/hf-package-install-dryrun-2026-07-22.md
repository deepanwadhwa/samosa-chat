# HF package + clean-machine install dry-run — 2026-07-22

Owner-gated release prep (owner chose "prep + dry-run now, you upload"). This
records the package build and a faithful clean-install dry-run of the **real**
release artifact, and the release bug the dry-run caught and fixed. The actual
Hugging Face upload is the owner's step (agent shells have no push credentials).

## Environment / safety

- macOS arm64, 16 GiB M3 Air. No model server running at the time (`server.pid`
  dead, no `qwen36b --serve`, no `samosa-gateway`); memory 78% free.
- `~/Documents/samosa-models` and the scratch/staging paths share one Data
  volume, so `package_hf.py` hard-links the 20.9 GB `experts.bin` + 3.0 GB
  `resident.safetensors` (no copy). The only heavy I/O is **reads** for
  checksums — reads do not consume SSD endurance (H1).

## Package

```
$ python3 tools/package_hf.py --gateway --out ~/Documents/samosa-hf-staging
...
staged 29 files, 23.99 GB -> ~/Documents/samosa-hf-staging   (~11 s, hard-linked model)
```

Staged: model container (experts/resident/manifest/config/generation_config,
hard-linked), tokenizer, `engine/*` sources incl. `samosa_gateway.c` +
`samosa_fs.c` (gateway), `install.sh`, `samosa`, `README.md` (= `MODEL_CARD.md`),
`app.html`, logo, plus `checksums.txt` and `release-manifest.tsv`. Manifest
passes `install.sh`'s safety awk (29 entries, all `sha\tsize\trelpath`).

## Clean-install dry-run (faithful, no 24 GB write, no model load)

Ran the shipped `dist/install.sh` against the real package over `file://`, with
the big model files pre-seeded as **hard-links** into the install staging dir so
`verified()` checksum-passes and skips their fetch, and `SAMOSA_INSTALL_TEST=1`
to exercise stage → verify → **compile** → atomic activate while skipping the
real-model serve smoke:

```
SAMOSA_HOME=<scratch> SAMOSA_BASE_URL=file://<staging> \
SAMOSA_INSTALL_TEST=1 SAMOSA_SKIP_PATH_SETUP=1 sh <staging>/install.sh
```

Result: install exit 0; manifest verified; `experts.bin`/`resident.safetensors`
verified-in-place and skipped (no copy); small sources fetched; engine compiled
(`qwen36b`), gateway compiled (`samosa-gateway`), jobs daemon compiled
(`samosa-jobsd`), fs sidecar compiled (`samosa-fs`); release activated atomically
(`current -> releases/<id>`); launcher written. The installed `samosa-jobsd`
dispatches its one-shot: `samosa-jobsd jobsd-once` → `{"ok":true,"decisions":[]}`
exit 0.

## Release bug found and fixed — `samosa-jobsd` was never installed

The **first** dry-run produced `qwen36b`, `samosa`, `samosa-fs`,
`samosa-gateway` — but **no `samosa-jobsd`**. `dist/install.sh` compiled the
gateway and fs sidecar but never built `samosa-jobsd`, while the launchd plist
the gateway installs runs `current/bin/samosa-jobsd`. So **the background
scheduler would be broken on every clean install** — it only worked on this
machine because a dev install (`install_local_dev.sh`) had placed the binary.
`tests/test_gateway_installer.sh` missed it (it asserted `samosa-fs` +
`samosa-gateway` only). The two binaries are not byte-identical (build metadata),
so a copy is not the fix.

Fix: `dist/install.sh` now compiles `samosa-jobsd` from `samosa_gateway.c`
alongside `samosa-gateway` (same source, launchd-friendly name), and
`tests/test_gateway_installer.sh` asserts `current/bin/samosa-jobsd` is
executable. Re-packaged and re-ran the dry-run: `samosa-jobsd` now present and
functional. `make test` and `make jobs-test` green.

Follow-up on the same release-integration class: when document support is
enabled, `dist/install.sh` now also smoke-tests the staged
`samosa-extract --json-pages` interface before activation. This prevents a
rebuilt gateway from shipping against a stale extractor binary.

## Before the owner uploads — required / open

1. **Re-package with `--repo-id <user/name>`.** This dry-run used the default
   `REPO_ID_PLACEHOLDER`; the real upload needs the repo baked into
   `install.sh` / `samosa` / `README.md` so `curl .../install.sh | sh` resolves.
2. **H1 published-claim defect is UNRESOLVED.** The packaged `README.md`
   (`dist/MODEL_CARD.md`) still states expert-streaming *reads* wear the SSD.
   Publishing now ships that disputed claim. This is an owner decision
   ([CLAUDE.md](../../../CLAUDE.md) published-claim defect / TASKS_HARDWARE H1).
3. **Documents (PDF) are disabled in this package** — no `--pdfium-dir` (the
   three SHA-reviewed PDFium archives were not present). For PDF support the
   owner must re-package with `--pdfium-dir <dir>`; the manifest then enables the
   extractor. Absence is clean (jobs return `extractor_unavailable`).
4. **One last-mile check not run here:** the real-model install serve smoke
   (`SAMOSA_INSTALL_TEST=1` skipped `install.sh`'s serve + generation smoke,
   which loads the 24 GB model). Recommend one full local install without that
   flag, or the equivalent, before or right after upload.

## Upload command (owner runs, with credentials)

```
python3 tools/package_hf.py --gateway --repo-id <user/name> \
  [--pdfium-dir <dir>] --out ~/Documents/samosa-hf-staging
hf upload <user/name> ~/Documents/samosa-hf-staging . --repo-type model
```

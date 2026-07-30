# Chutni submodule upgrade: 0fc912c → 04a7451 (v0.3.0, spec 0.2)

**Date:** 2026-07-30
**Branch:** `ui-chutni`
**Machine:** one 16 GB M3 MacBook Air, macOS Darwin 25.5.0 arm64, Apple clang
(Xcode 26.5.0), APFS. **The only machine any of this ran on.**
**Model:** none. Every gate here is offline, against stub backends and small
fixtures. Nothing below was run against the real 24 GB Qwen.

## Why

`vendor/chutni` was pinned to `0fc912c`, a commit that **does not exist in
`chutni-protocol`**. Its parent `c30c49c` is an ancestor of `main`, but
`0fc912c` itself lived only inside this repository's submodule checkout, whose
`origin` pointed at the local path `/Users/deepanwadhwa/Documents/chutni-protocol`
rather than the `github.com/deepanwadhwa/chutni-protocol.git` URL declared in
`.gitmodules`. A fresh `git clone --recursive` of Samosa could not have fetched
its own pin.

`main` was 22 commits ahead: implementation `0.3.0`, spec `0.2`, 17 MCP tools
against the vendored 10.

## What the pin actually was

Every distinctive field and error code `0fc912c` introduced is present on
`main` — checked 15 markers (`report_progress`, `scan_progress`,
`confirmation_required`, `supersedes_artifact_id`, `deterministic_transform`,
`source_not_current`, `root_not_authorized`, `summary_short`, `recipe_hash`,
`producer_kind`, and the six contract counters). It was superseded, not lost.
The authoritative check is the consumer-side one below, which passes.

`git ls-remote` confirms GitHub carries `main` at `04a7451`, so moving the pin
forward is also what fixes the orphan. `git submodule sync` reset the
checkout's `origin` to the declared URL, and the fetch that moved the pin was
made **from that URL**, not from the local path.

## The upgrade path per docs/SAMOSA-COMPATIBILITY.md

Steps 3–5 were run. Step 2 (`make test-samosa-compat` inside Chutni) was **not
run here** — the Chutni side reports it green at `04a7451`, and this work went
straight to the consumer-side authority that doc names as final.

| Gate | Result |
|---|---|
| `make chutni-gateway-test` (baseline, old pin) | PASS |
| `make chutni-gateway-test` (candidate binary) | PASS |
| `make test` (see caveat below) | **exit 0** — [make-test.txt](make-test.txt) |

Individually re-run after the pin moved: `compiled-gateway-test`,
`test-chutni-db`, `test-chutni`, `chutni-gateway-test`,
`detached-service-test`, `test-ui-setup`, `test_chutni_views.sh`,
`test_hidden_toggles.sh`, `test_attachments.sh` — all PASS.

## Spec 0.1 → 0.2, run against a Samosa-shaped store for the first time

SPEC claims v0.1 stores stay readable and are not rewritten on open. That is
conformance-tested inside Chutni but had never been run against a store Samosa
wrote. Full transcript: [spec-0.1-store-compat.txt](spec-0.1-store-compat.txt).

- **Read-only holds.** `chutni_store_info`, `chutni_list_sources`,
  `chutni_search`, `chutni_folder_status` all return `ok:true` against a 0.1
  store, and all three store files are **byte-identical by SHA-256**
  afterwards. The manifest still says `spec_version: 0.1`.
- **The first write upgrades in place** to `0.2`, adding the
  `hierarchical_sources`, `bounded_coverage`, and `directory_definitions`
  capabilities.
- **Rollback survives.** The old 0.1 binary still opens and searches the
  upgraded store, because the §35 reader gate rejects only `major > 0`
  (`src/chutni.c:788-795`).

## Three defects the upgrade introduced on Samosa's side, and the fixes

### 1. Chutni 0.2 metadata artifacts were reaching the model's prompt

The 0.2 scanner writes an indexed `file_metadata` artifact for **every** file
(`{"size_bytes":N,"depth":N}`), plus `directory_listing` per enumerated
directory. These rank in `chutni_search` beside real content.
`chutni_chat_evidence` filtered only on a non-empty snippet and
`freshness == "current"`, so they qualified.

Measured on a four-file folder: a six-hit search returned **three
`extracted_text` and three `file_metadata`** — half the evidence budget spent
on JSON bookkeeping, paid for in prefill, crowding out the file text the user
asked about.

Fixed in `chutni_content_artifact()` (`src/samosa_gateway.c`): an allowlist of
the five content kinds Samosa is willing to splice into a prompt
(`extracted_text`, `page_text`, `ocr_text`, `image_caption`, `summary_short`).
Applied to both the chat evidence path and `POST /v1/chutni/query`, whose
`used`/`reason_code` is now computed from what survives the filter rather than
from what the index returned. Both call sites over-fetch (30) and trim, because
a limit of six would let the metadata starve the evidence rather than merely
dilute it.

An allowlist, not a denylist, because the failure directions are not symmetric:
an unrecognized content kind is missing evidence, which shows up in the answer
and in the gate below; an unrecognized kind admitted by default is silent
prompt pollution.

Regression test added to `tests/test_chutni_gateway.sh`. It asserts the store
genuinely contains `file_metadata` artifacts (via SQLite) and that a query
matching only bookkeeping returns `used:false` with no `size_bytes`,
`file_metadata`, or `directory_listing` in the response. **Verified to fail
without the fix**: stubbing `chutni_content_artifact()` to return 1 made
`make chutni-gateway-test` exit 1; restoring it returned it to PASS.

**Note for the compatibility contract:** Samosa now depends on `artifact_kind`
in search hits. `docs/SAMOSA-COMPATIBILITY.md` lists `source_id`,
`artifact_id`, `display_path`, `snippet`, and `freshness` as the guaranteed
fields — not `artifact_kind`. That doc should be extended, or Samosa is relying
on an ungoverned field.

### 2. A user-visible counter silently changed meaning

`scan.metadata_artifacts` went **1 → 4** on the same four-file folder, because
0.2 records a metadata artifact per file rather than only for unreadable ones.
Samosa surfaced it as "Metadata-only at scan", which would have read 4-of-4 on
a folder with one metadata-only file. Full comparison:
[counters-old-vs-new.txt](counters-old-vs-new.txt).

Relabelled to "File records written" in `assets/app.html`. The honest
metadata-only number is the existing "Metadata only" fact, which comes from
`counts.metadata_only_sources` and is **unchanged** at 1.

Every other counter in the compatibility contract holds its file-only meaning:
`sources_indexed` 4→4, `content_artifacts` 3→3, `content_readable_sources`
3→3, `metadata_only_sources` 1→1, and `sources_files` appears (→4).
`counts.sources` 4→6 and `artifacts_active` 4→10 both inflate with directory
sources; Samosa ignores the first, and uses the second only as a fallback that
does not fire while `metadata_only_sources` is present.

### 3. A released Samosa would have written a fake version into every store

Chutni 0.3.0 compiles its release version in from `VERSION` and stamps it into
the producer record of every artifact (§16.1). `dist/install.sh` compiles the
service by hand rather than through Chutni's Makefile and did not pass
`-DCHUTNI_VERSION`, so the in-source fallback applied. Measured on a real
staged install: the shipped binary recorded

```
chutni-reference-scanner|0.0.0-unversioned
```

against `0.3.0` from the `make`-built binary — permanently, in the user's own
store, attributing artifacts to a build that never existed.

Fixed by shipping `engine/chutni/VERSION` (`tools/package_hf.py`) and passing
`-DCHUTNI_VERSION` from it (`dist/install.sh`), which fails loudly if the file
is absent rather than silently shipping the fallback. Verified on a real
package-serve-install cycle: the installed binary now records `0.3.0`.
Asserted permanently in `tests/test_runtime_only_release.sh`, against the
**store** rather than the binary's help output, because the store is where it
does damage.

## Pre-existing defect found while testing — NOT fixed, NOT related to Chutni

`make test` fails on this machine before any of this work.
`tests/test_runtime_only_release.sh` aborts with

```
curl: (22) The requested URL returned error: 500
[samosa] ERROR: staged model catalog endpoint smoke failed
```

**Confirmed pre-existing:** the identical failure reproduces at `HEAD` with the
old pin and all of these changes stashed.

**Root cause:** `dist/samosa`'s launchd plist enumerates an explicit
`EnvironmentVariables` dict (`dist/samosa:261-273`) that omits
`SAMOSA_MODELS_CATALOG`. A launchd job receives only that dict — nothing is
inherited. `dist/install.sh`'s smoke test deliberately isolates `SAMOSA_HOME`
to `$STAGE/.smoke-home` while `models.json` stays at `$STAGE/models.json`, and
passes the location via `SAMOSA_MODELS_CATALOG`; launchd drops it, the gateway
falls back to `$SAMOSA_HOME/current/models.json`, which does not exist, and
`/v1/models/catalog` returns 500 `catalog_missing`.

Decisive check: same command, same staged release, `HTTP 500` normally and
`HTTP 200` with `SAMOSA_DISABLE_LAUNCHD=1`.

**Every gate in this document was therefore run with
`SAMOSA_DISABLE_LAUNCHD=1`.** With that set, `make test` exits 0. Without it,
`make test` fails for this unrelated reason.

Two candidate fixes, needing an owner decision because both touch release
machinery:
1. Add `SAMOSA_MODELS_CATALOG` to the plist and pass it through from the
   caller's environment.
2. Have the installer's smoke set `SAMOSA_DISABLE_LAUNCHD=1` — arguably the
   more correct one, since a staged, inactive, partial release probably should
   not be registering a launchd job at all.

## Explicitly not done

**`chutni_put_memory` was not adopted.** The prior session's plan was to store
Samosa conversation and work memory through it. The owner rejected that scope
on 2026-07-30: Chutni stores memory of files, filesystems and directories, not
Samosa chat. `docs/SAMOSA-COMPATIBILITY.md` says the same thing in its standing
rule — *"Chutni must not write Samosa chat state."* No memory work was built.

For the record, since it was measured before the scope was corrected: the tool
works, and its argument shape is not what the compatibility doc implies. It
needs `store_path`, `memory_kind`, `text`, `operation`, `confirmed`, and a
nested **`producer` object** whose own required fields are `producer_kind` and
`name` (§16.1) — not the flat `producer_kind`/`producer_name`/`model_id` used
by `put_derived_artifact` and `put_model_artifact`.

**`src/samosa_chutni_db.c` was not removed.** 776 lines, the legacy SHA-256
sidecar holding `memory_cards`. Its only references are its own Makefile build
rule and its four test scripts; no application source calls it. It is still
gated in `make test` via `test-chutni-db`. Deleting it is destructive cleanup,
which the non-negotiables reserve for explicit confirmation.

## What "works" does and does not mean here

Per the non-negotiables: this is **tests pass**, not **works**. The upgrade has
not been exercised against the real 24 GB Qwen, and no real user folder has
been rebuilt with the 0.2 scanner. The evidence-filter change alters what
reaches a live model's prompt, and that has only been observed against the fake
backend. A real-model dogfood on a real folder is still owed.

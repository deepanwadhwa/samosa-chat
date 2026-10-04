# Native folder-memory throughput — 2026-10-03

Downloads, its contents, the user's PDF, and existing conversations were not
opened for inspection or testing. All document inputs came from independently
generated Opening Samples fixtures. Runtime inspection used health and scope
control state only. The prior user build was already `canceled_initial`.
Scheduling and the wider FW-6 acceptance gates remain unchanged.

## Shipped pipeline

- C worker pool: six workers on hosts with at least eight logical cores,
  four on smaller hosts with at least four cores, otherwise two. The bounded
  `SAMOSA_CHUTNI_WORKERS=1..6` override allows reproducible comparisons.
- Workers overlap PDF/text/OCR extraction, queued summarization and storage.
  In-flight work is bounded by the pool, with at most 100 source descriptors
  in each inventory page. This does not spawn Python workers.
- One resident C++ T5 process shares one model between legacy scalar and
  reusable batched inference contexts. A coordinator coalesces ready requests
  over four milliseconds; it never waits for a full batch. Two isolated
  encoder/decoder sequences run together in the pinned native runtime with
  unified KV storage and per-sequence cross-attention masks.
- Opening samples still target 3,000 Unicode characters and keep whole units
  that cross the target. All collected sample bytes reach the summarizer.
  Its actual tokenizer splits them at the 512-token encoder boundary rather
  than making extra inference calls based on a conservative byte estimate.
  Memory digests generate up to 64 tokens per mapped passage. Repetition of a
  complete substantial sentence ends that sequence instead of consuming the
  rest of its output budget. No input is dropped by this loop detector.
- Each file's parser/model outputs retain their individual provenance and
  commit in one transaction. The source's expected BLAKE3 identity is checked
  once for the batch. No per-page refresh/cascade or whole-store search-index
  rebuild is needed. Reused artifacts are retained; replacements supersede
  prior matching outputs. Any invalid output rolls the entire batch back.
- Native child processes close unrelated inherited descriptors. Memory reader
  cancellation is independent of interactive attachment-reader cancellation.
- Progress reports all active files in a table with stage/page/characters,
  queued summaries, completed files, and per-stage timing counters. Worker
  latency totals overlap and must not be added to derive wall-clock time.

File-byte hashing still occurs during cataloging, reader cache identity, and
final version verification. The sampling target bounds content extraction,
not those identity checks. OCR work depends on pixel size/layout and page
count, so a fixed character target cannot guarantee equal time per file.

## Final measurements

The final cold cases use fresh stores, empty reader caches, and unique bytes
for every file. Earlier trials using identical copies could reuse extraction
caches; their rates are not used as the final cold results. PDF/image layouts
are repeated to hold difficulty constant, with unique trailing markers to
prevent content-addressed cache reuse. Text inputs have distinct prefixes.

| Case | Files | Time | Files/s | Summaries | Failures | Peak process-tree RSS |
|---|---:|---:|---:|---:|---:|---:|
| Previous installed pipeline, unique mixed files | 24 | 61.118 s | 0.393 | 20 | 0 | 214.9 MiB |
| New native pipeline, unique text/native PDF | 32 | 12.911 s | 2.479 | 32 | 0 | 266.2 MiB |
| New native pipeline, unique mixed/OCR | 24 | 15.940 s | 1.506 | 20 | 0 | 687.9 MiB |

The matched mixed workload improves approximately 3.83x. Its four blank PDFs
correctly receive no summaries. OCR-heavy throughput remains below the desired
2–3 files/s; these results do not promise that rate for arbitrary scanned pages.
RSS sampling includes the helper's process tree and excludes the running chat
backend/UI. Summarization timings include queue latency. Baseline measurements
ran alongside regression checks and are not a controlled hardware benchmark.

A separate store-size probe used 3,054 preexisting generated catalog artifacts
and processed 24 mixed files in 15.585 seconds, with zero failures. Storage
accounted for 1.417 seconds of summed worker latency; the actual commit work
was 193 milliseconds. This probe repeated layouts and may reuse reader caches;
it demonstrates bounded storage overhead, not independent cold-OCR speed.

## Verification and reproduction

- Compiled gateway regression gate passes, including attachment cancellation,
  bounded reader contracts, web search, developer traces, and no-Python startup.
- Chutni gateway, pause/resume and crash/recovery gates pass.
- Chutni native core/MCP/binding suite passes; the new tool is advertised.
- Generated concurrency test checks the worker cap, simultaneous active rows,
  faster independent extraction, summary provenance, artifact reuse, atomic
  rollback and rejection of changed source bytes.
- Native summarizer supervisor checks one persistent process and eight
  concurrent callers receiving their own results. The real T5 test sends
  distinct subjects/numbers in two batches and rejects cross-file leakage.
- UI DOM tests render the active-files table. Browser layout/paint has not
  been visually verified; the session's browser runtime was unavailable.

Reproduce a cold run with the existing generated fixture directory:

```sh
make samosa-gateway samosa-summarizer test-chutni-sampling
python3 tools/benchmark_chutni_throughput.py \
  --fixtures "$HOME/Documents/Samosa Walkthrough/Opening Samples" \
  --output /private/tmp/samosa-throughput-fresh --workers 6
```

The benchmark driver is test-only Python orchestration. The shipped pipeline,
OCR, storage, and summarizer use C/C++; no new application Python dependency
or Python worker pool was added.

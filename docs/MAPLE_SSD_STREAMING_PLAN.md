# Maple SSD-streaming engine plan

**Status:** M0-M5 correctness path complete: production loading is fail-closed,
all 24 MoE layers stream from SSD, prefill/decode memory is bounded, and the
real-checkpoint frozen greedy gate is 10/10 prompts and 320/320 tokens exact
**Date:** 2026-08-11
**Target machine:** Apple M3, 16 GB unified memory
**Safety state:** the default Maple loader is fail-closed without validated
streaming artifacts and loads the resident-only plus SSD-streamed path when
they are present. Full-resident loading remains an explicit developer-only
escape hatch through `SAMOSA_MAPLE_ALLOW_FULL_RESIDENT=1` and must stay behind
a hard memory guard.

## 1. Outcome

Replace Maple's current full-resident MLX expert tensors with a Samosa-native,
byte-budgeted SSD expert store. The engine must retain only resident tensors in
MLX memory, route each token, fetch the selected experts from SSD through the
existing `expert_cache` policy, evaluate them, and evict them under a strict
budget.

The result must preserve Maple's greedy output while keeping memory bounded on
the 16 GB reference Mac. Runtime inference must remain native; no Python
interpreter, Python package, or Python subprocess may be shipped or invoked.

This is a runtime redesign, not a wrapper around the current `all_weights`
loader.

## 2. Current state and confirmed defect

### Runtime architecture

- Qwen uses Samosa's native `qwen36b` C engine. Its routed experts live in
  `experts.bin` and are loaded with `pread` into a byte-budgeted LRU managed by
  `src/expert_cache.{h,c}`.
- Maple uses a separate C++/MLX executable, `samosa-maple`.
- Ornith and Bonsai use the shared `llama-server` backend.
- `samosa-gateway` supervises one active backend process and proxies the common
  HTTP surface, but there is no shared inference-runtime implementation.

Separate model kernels are reasonable; a completely separate memory policy,
I/O path, telemetry model, and safety envelope are not. Maple must reuse the
Samosa cache policy and resource rules even if its MLX compute kernels remain
model-specific.

### Baseline defect (resolved)

The original `load_maple_model()` opened every safetensor shard, inserted every
tensor into `all_weights`, and gave the complete map to `MapleModel`. Every
layer's `MapleSparseMoeBlock` retained three full 256-expert projection tensors
and their scale data. The first real inference then materialized those lazy
arrays and MLX work buffers in unified memory.

The production loader now replaces that path with a validated expert manifest,
exact `pread`, bounded cache admission/eviction, pressure handling, and
expert-I/O telemetry. Full-resident loading is quarantined behind an explicit
developer-only environment switch.

### Measured checkpoint split

The three Maple safetensor shards contain:

| Class | Bytes | GiB |
|---|---:|---:|
| Routed expert tensors | 4,869,586,944 | 4.535 |
| Resident tensors | 438,599,680 | 0.408 |
| Total tensor payload | 5,308,186,624 | 4.944 |

Each layer has 256 experts and routes eight per token. One expert contains:

- gate packed U32 weights: `512 x 128`, 262,144 bytes;
- up packed U32 weights: `512 x 128`, 262,144 bytes;
- down packed U32 weights: `2048 x 32`, 262,144 bytes;
- gate/up/down BF16 `row_alpha`: 6,144 bytes total.

That is 792,576 logical bytes per expert, or 802,816 bytes when rounded to a
16 KiB record boundary. The production default holds at most 96 records
globally (four entries per layer equivalent), exactly 77,070,336 bytes or
about 73.5 MiB of expert payload. This is a fixed ceiling, not an automatically
growing target.

## 3. Non-negotiable requirements

1. **SSD streaming:** routed expert payloads are fetched only after routing.
2. **Bounded memory:** expert payload memory has one explicit byte budget and
   cannot grow beyond it.
3. **No full-resident fallback:** the 16 GB product path must never silently
   fall back to loading all expert tensors.
4. **Exact weights:** stream the checkpoint's existing packed 2-bit weights and
   BF16 row scales without requantizing them.
5. **Native runtime:** no Python dependency or subprocess in installation or
   inference. An offline developer converter is permitted only if the shipped
   artifacts are already converted; the preferred packer is native C++.
6. **Output parity:** streamed and full-resident reference paths must agree on
   router choices, expert outputs within defined numeric tolerance, and greedy
   token sequences.
7. **Safe MLX lifetimes:** an expert cache entry cannot be evicted while a lazy
   MLX graph still references its bytes.
8. **Bounded prefill:** prompt prefill must not materialize every expert touched
   by a long prompt at once.
9. **Pressure response:** warning and critical memory pressure must reclaim
   expert cache and reusable scratch allocations.
10. **Honest telemetry:** health and generation metadata report cache budget,
    payload/peak bytes, hit/miss counts, evictions, SSD bytes read/avoided,
    current process footprint, and swap delta.

## 4. Target architecture

```text
samosa-gateway
    |
    | common backend HTTP contract
    v
Maple native service
    |
    +-- resident MLX model (~0.408 GiB checkpoint payload)
    |     attention, routers, norms, embeddings, output head
    |
    +-- MapleExpertStore
          |
          +-- validated manifest/index
          +-- exact pread + bounded aligned scratch
          +-- expert_cache policy (LRU, byte budget, pressure reclaim)
          +-- pinned MapleExpertLease objects
          +-- MLX no-copy tensor views for gate/up/down + row_alpha
```

### Shared versus model-specific code

Reuse unchanged where possible:

- `src/expert_cache.{h,c}` for byte accounting, LRU, eviction, pressure, and
  telemetry;
- the Qwen principles of manifest validation, exact `pread`, aligned slabs,
  compute-and-discard on failed admission, fixed cache defaults, and explicit
  pressure handling;
- gateway backend lifecycle and the `/healthz` + `/v1/chat/completions`
  contract.

Add shared runtime helpers rather than copying them into Maple:

- exact EINTR-safe `pread`;
- host memory-pressure query;
- current-footprint and swap telemetry;
- cache-budget parsing and validation;
- aligned slab allocation/recycling.

Keep model-specific:

- Maple manifest record interpretation;
- packed U32/BF16 projection views;
- Maple router, ternary quantized matmuls, SwiGLU, and aggregation;
- attention/KV-cache implementation.

## 5. On-disk format

### Phase-A prototype: direct safetensor slices

First prove the runtime without creating a second 5 GB copy of the model.
A native index builder reads only each safetensor JSON header and records the
six expert regions per layer:

- `gate_proj.weight`, `gate_proj.row_alpha`;
- `up_proj.weight`, `up_proj.row_alpha`;
- `down_proj.weight`, `down_proj.row_alpha`.

Each expert is contiguous within each source tensor because expert is the
leading dimension. A miss performs six bounded `pread` calls into one aligned
slot. This path is a correctness and memory prototype, not the final release
format.

The direct-slice manifest must include, for every region:

- source shard identity and expected size;
- absolute byte offset and byte length;
- dtype and exact per-expert shape;
- layer, expert, and projection identity;
- SHA-256 of the source artifact and optionally each expert record for strict
  development validation.

### Production format: contiguous `maple-experts.bin`

After Phase A passes, build one canonical expert container with one aligned
record per `(layer, expert)`. Suggested record order:

1. fixed header with schema/version/record length;
2. gate packed weight, then gate `row_alpha`;
3. up packed weight, then up `row_alpha`;
4. down packed weight, then down `row_alpha`;
5. zero padding to 16 KiB alignment.

Artifacts:

- `maple-resident.safetensors` — all non-expert tensors only;
- `maple-experts.bin` — 24 x 256 aligned expert records;
- `maple-manifest.json` — format, dimensions, offsets, lengths, artifact
  digests, quantization contract, and source checkpoint identity;
- existing `config.json` and tokenizer assets.

The native packer must stream source regions and never map/materialize the full
checkpoint. It writes to temporary files, fsyncs, validates all sizes and
digests, then atomically renames the completed artifacts. It must reject
overlaps, holes, duplicate experts, wrong dtypes/shapes, truncated input, and
integer overflow.

The source safetensor shards are developer/import inputs. A released streaming
installation should not require them once the resident/expert artifacts have
been produced.

## 6. Runtime design

### 6.1 Resident model load

Change `load_maple_model()` so it never retains `switch_mlp` expert tensors.
It loads only:

- router gates;
- attention projections and norms;
- embeddings and output head/FlashHead data;
- final norms and other small resident parameters.

`MapleSparseMoeBlock` retains its router and a `(MapleExpertStore*, layer_id)`
reference, not three full `QLinear` expert stacks.

The loader must fail closed if the streaming manifest/store is absent or
invalid. The old full-resident path may remain temporarily behind a
development-only compile flag for parity tests, but it is not installed,
catalogued, or selected by the app.

### 6.2 Expert store and cache payload

Add a C++ `MapleExpertStore` around `expert_cache`.

Each cache payload owns:

- one aligned slab containing exactly one expert record;
- layer/expert identity and logical/source byte counts;
- no-copy MLX arrays for the three packed weights and three `row_alpha`
  vectors, or enough metadata to create those views on demand;
- a pin/reference count used by active inference leases.

Default policy:

- LRU;
- 96 entries globally, or four entries per layer equivalent (~73.5 MiB aligned
  expert payload);
- two-entry per-layer floor only if measurements show it helps without
  violating pressure reclaim;
- explicit `SAMOSA_MAPLE_EXPERT_BUDGET_MB` override with validated minimum and
  hard maximum;
- no automatic cache growth based on free RAM.

Cache admission failure is not fatal: evaluate the freshly loaded expert and
discard it after use.

### 6.3 MLX lifetime and eviction safety

MLX evaluates lazily, so a raw pointer returned from `ecache_get()` is not safe
unless its lifetime spans graph execution.

Implement an explicit `MapleExpertLease`:

1. lookup/load and pin the cache payload;
2. create no-copy MLX arrays with a lifetime tied to the lease;
3. run expert matmuls;
4. force evaluation of the expert contribution before releasing the lease;
5. unpin, then permit eviction/reclaim.

The generic cache currently has no pin API. Add and test `ecache_pin()` and
`ecache_unpin()` (or an equivalent non-evictable lease mechanism) rather than
depending on call ordering. Automatic eviction must skip pinned entries;
explicit destruction waits until inference is quiescent.

The backend admits one generation at a time with a model-wide inference mutex.
Health checks remain concurrent and must not touch MLX model state.

### 6.4 Decode path

For each layer and generated token:

1. compute router scores and the exact top eight IDs/weights;
2. acquire leases for the required experts, loading misses with `pread`;
3. evaluate gate/up/down quantized matmuls using each expert's no-copy views;
4. aggregate in the same numeric order and dtype as the full-resident baseline;
5. evaluate the layer contribution;
6. release leases;
7. admit loaded misses or compute-and-discard according to the cache result.

Do not use `take()` over a resident `[256, ...]` tensor; that tensor must no
longer exist.

### 6.5 Bounded prefill path

Prefill can route prompt rows to many unique experts. It needs a grouped
algorithm modeled on Qwen's `moe_forward`, not the decode-only eight-expert
assumption:

1. route all rows and record top-k IDs/weights;
2. build deterministic unique-expert groups;
3. process experts in bounded batches;
4. gather only rows assigned to the current expert;
5. load/lease that expert, run its projections, and accumulate output;
6. force evaluation before releasing the expert;
7. continue until all groups are complete.

Scratch memory is sized by an explicit token microbatch and expert-batch cap.
Long prompts are split into bounded prefill chunks. The accumulation order is
fixed to keep parity reproducible.

Do not enable whole-layer sequential reads by default. Qwen measurements found
that kernel readahead plus scattered `pread` was faster; Maple must earn a
different policy with its own measured cold-cache A/B.

### 6.6 Pressure and cancellation

At safe boundaries before cache lookup—not while a lease is live:

- normal: no action;
- warning: reduce cache toward 75% of its current payload and trim unused slab
  pools;
- critical: reclaim all unpinned expert entries and scratch pools;
- cancellation/shutdown: finish or cancel outstanding MLX work, release leases,
  destroy the cache, close descriptors, and return memory to the OS.

The process must not be restarted automatically after a memory-safety failure.
The gateway reports the failure and leaves Maple unavailable until explicitly
selected again after correction.

## 7. Implementation milestones

### M0 — Quarantine the unsafe path

- Keep Maple stopped during development.
- Prevent the app from selecting the full-resident path on normal builds.
- Put any retained baseline path behind a test-only build flag and the existing
  hard memory guard.
- Add a regression asserting a production Maple build cannot start without a
  streaming manifest/store.

**Exit:** no user-facing action can accidentally launch the current 10 GB path.

### M1 — Manifest/index and byte-exact extraction

- Implement a native safetensor-header parser/indexer.
- Validate all 24 x 256 expert slices and resident/expert classification.
- Implement exact six-region `pread` into one aligned expert slab.
- Compare every extracted expert against the corresponding source slices by
  SHA-256 and byte equality on representative and exhaustive offline tests.
- Record the measured 4.535 GiB expert / 0.408 GiB resident split in test
  output.

**Exit:** any expert can be fetched independently with bounded memory and
validated bytes; no model inference yet.

### M2 — Reusable cache/lease layer

- Link Maple against `src/expert_cache.c`.
- Add pin/unpin semantics and tests to the generic cache.
- Implement aligned slab pooling with a small hard cap.
- Add cache budget parsing, per-layer keys, stats, pressure reclaim, and clean
  destruction.
- Add multithreaded lifecycle tests around lookup, pin, cancellation, and
  shutdown; cache mutation itself remains serialized.

**Exit:** synthetic Maple-sized expert records cannot exceed the configured
payload budget under randomized lookup/eviction/pressure sequences.

### M3 — One streamed expert, one layer

- Replace one layer's resident switch tensors with `MapleExpertStore` lookups.
- Build no-copy U32/BF16 MLX views over a pinned expert slab.
- Compare gate/up/down outputs and the weighted expert contribution against the
  full-resident layer across fixed vectors, extreme values, and all three
  projections.
- Verify the MLX graph is evaluated before unpin/eviction.

**Exit:** streamed and resident layer outputs match within a declared tolerance
and sanitizer/lifetime tests find no use-after-free.

### M4 — Full decode streaming

- Convert all 24 MoE layers.
- Remove full expert arrays from production model objects.
- Add cache hit/miss/admission/eviction telemetry.
- Verify router IDs/scores and greedy token sequences against frozen resident
  references.

**Exit:** 1, 8, 32, and 128-token greedy runs pass without loading expert
stacks; process memory remains bounded.

### M5 — Bounded prefill

- Implement deterministic row grouping by expert.
- Add prompt chunking and bounded expert batches.
- Test empty/short prompts, repeated experts, all-unique synthetic routes,
  sliding/full-attention boundaries, and cancellation during prefill.
- Measure prefill memory independently of decode memory.

**Exit:** long-prefill memory stays under the same hard process ceiling and
prompt+generation token parity passes.

### M6 — Canonical release artifacts

- Implement the native streaming packer.
- Produce `maple-resident.safetensors`, `maple-experts.bin`, and a strict
  manifest.
- Add resumable staging, checksums, atomic finalization, and corruption tests.
- Update the catalogue/installer so final releases hard-link or download these
  artifacts and do not ship Python.

**Exit:** a clean install can run Maple using only final streaming artifacts.

### M7 — Gateway and product integration

- Define one backend capability record for executable, model artifacts,
  health path, image/document support, and runtime settings; remove Maple-only
  availability/metadata conditionals where the generic descriptor suffices.
- Expose Maple cache budget and context settings through the existing runtime
  settings API with safe locked defaults on the 16 GB tier.
- Return cache/I/O/memory telemetry through the same generation metadata shape
  used by Samosa's native Qwen runtime.
- Make startup failure, pressure shutdown, and selection rollback explicit in
  the UI.

**Exit:** Maple behaves as a first-class Samosa backend, not an unobserved
special-case process.

### M8 — Remove the full-resident implementation

- Delete production loading of `switch_mlp` expert stacks.
- Retain only a fixture-scale resident oracle for tests.
- Remove obsolete shard staging and old release artifacts after a recoverable
  migration window.

**Exit:** repository and installed releases have one production Maple path:
SSD-streamed experts.

## 8. Verification matrix

### Format and I/O

- malformed JSON/header length;
- missing/duplicate expert;
- wrong dtype, shape, quantization metadata, offset, length, or alignment;
- overlapping/out-of-range slices;
- truncated expert store and short `pread`;
- SHA mismatch;
- EINTR retry and descriptor-close behavior;
- source/final artifact byte-for-byte conversion checks.

### Cache policy

- exact byte-budget enforcement including alignment charge;
- hit/miss/avoided-byte counters;
- LRU order and stable tie behavior;
- failed admission compute-and-discard;
- pin prevents eviction;
- unpin enables pending pressure reclaim;
- warning/critical reclaim and slab-pool trimming;
- clean destroy with zero live leases.

### Numerical correctness

- per-projection quantized matmul parity;
- clamped SwiGLU parity;
- router top-eight IDs, ordering, and normalized scores;
- per-layer aggregate parity for decode and multi-row prefill;
- attention/KV boundary tests remain green;
- token-exact greedy parity on frozen fixtures;
- real-checkpoint greedy parity for representative short and long prompts.

### Lifecycle and API

- start/health/select/generate/cancel/stop;
- concurrent health during generation;
- second generation receives busy/backpressure rather than sharing model state;
- selection rollback after malformed store or startup failure;
- no stale process after stop;
- no hidden fallback to full-resident loading;
- no Python executable/library/process in the installed runtime.

### Memory and performance

All real-model tests run through a guard that kills the process tree on hard
limits and records baseline/final swap.

Measured/release gates on the 16 GB M3:

- expert-cache payload never exceeds its configured budget;
- default cache exactly 77,070,336 payload bytes (~73.5 MiB) plus bounded
  resident tensors and scratch space;
- idle-after-load footprint target below 1.5 GiB;
- short-generation peak target below 3.0 GiB;
- 8k-context peak hard ceiling 4.0 GiB pending measurement;
- swap growth below 256 MiB;
- no macOS warning/critical pressure event during the release smoke test;
- cache misses, reads, evictions, hits, and avoided bytes remain internally
  consistent even when the deliberately tight default has no warm hits;
- cold and warm token/s, bytes read/token, and energy/thermal observations are
  reported, not hidden.

If a target is missed, stop and investigate. Do not relax the ceiling merely
to make the test pass.

## 9. Planned file changes

Likely new files:

- `src/maple/maple_expert_store.h`
- `src/maple/maple_expert_store.cpp`
- `src/maple/maple_expert_manifest.h`
- `src/maple/maple_expert_manifest.cpp`
- `src/runtime/expert_io.h`
- `src/runtime/expert_io.c`
- `tools/pack_maple_streaming.cpp`
- `tests/test_maple_expert_manifest.cpp`
- `tests/test_maple_expert_store.cpp`
- `tests/test_maple_streaming_parity.cpp`
- `tests/test_maple_streaming_lifecycle.sh`
- `tests/test_maple_streaming_memory.sh`

Likely modified files:

- `src/expert_cache.{h,c}` — pin/unpin semantics;
- `src/maple/maple_model.{h,cpp}` — resident-only model and streamed MoE;
- `src/maple/samosa_maple.cpp` — admission, telemetry, clean lifecycle;
- `src/samosa_gateway.c` — generic backend capabilities and Maple settings;
- `Makefile` — shared cache/runtime objects, packer, and tests;
- `assets/models.json` — streaming artifacts and hashes;
- `tools/install_local_dev.sh` and release installer — artifact staging;
- `tools/run_memory_guarded.sh` — process-tree termination and Maple-specific
  guard profile;
- `README.md` / model documentation — measured memory and I/O only after gates
  pass.

## 10. Sequencing and review gates

The dependency order is strict:

1. quarantine unsafe launch;
2. prove byte-exact independent expert reads;
3. prove cache budget and MLX lifetimes with synthetic records;
4. prove one-layer numerical parity;
5. prove full decode parity;
6. prove bounded prefill;
7. package final artifacts;
8. integrate gateway/catalogue;
9. run guarded real-model release tests;
10. remove the old path.

Do not start performance tuning, asynchronous prefetch, custom Metal expert
kernels, or co-activation layout work before correctness and memory gates pass.
Those are follow-up optimizations, not prerequisites for a safe engine.

## 11. Definition of done

Maple is ready to return to the Samosa catalogue only when all of the following
are true:

- the production loader cannot load all routed experts;
- real inference streams selected experts from SSD through the shared cache;
- memory remains within the guarded ceilings on the reference M3;
- prefill and decode are both bounded;
- parity gates pass against frozen and real-checkpoint references;
- cancellation, shutdown, and pressure reclaim release memory cleanly;
- cache and SSD-I/O telemetry are visible and accurate;
- installer/runtime contain no Python dependency;
- malformed or missing streaming artifacts fail closed;
- the old full-resident Maple path is absent from production builds.

## 12. Current implementation checkpoint — 2026-08-09

This section is a handoff point for resuming work without relying on chat
history.

### Completed in the current work session

- `src/expert_cache.{h,c}` now has entry-scoped `ecache_pin()` and
  `ecache_unpin()` APIs.
- `ecache_view` reports `pin_count`.
- Automatic residual demotion, base eviction, admission planning, and pressure
  reclaim skip pinned entries.
- Explicit `ecache_remove()` and `ecache_destroy()` fail with
  `ECACHE_ERR_POLICY` while pins are live, so callers can wait for inference
  quiescence before freeing payload bytes.
- `tests/test_expert_cache.c` covers pinned eviction, pressure reclaim,
  explicit remove/destroy rejection, refcounted unpinning, and cleanup after
  release.
- `src/maple/maple_expert_store.{h,cpp}` now has:
  - `MapleExpertCacheConfig`;
  - `MapleExpertLease` RAII objects;
  - `enable_cache()`, `disable_cache()`, `cache_enabled()`;
  - `acquire_expert()`;
  - `apply_cache_pressure()`;
  - `cache_stats()`.
- `MapleExpertStore::acquire_expert()` uses the shared `expert_cache` policy:
  cache hits are pinned and returned as cached leases; misses are read with
  exact `pread`, admitted when possible, pinned, and returned; failed admission
  returns an uncached compute-and-discard lease.
- `tests/test_maple_expert_store.cpp` covers cache hit/miss behavior, live
  lease pinning, pressure skipping pinned experts, reclaim after lease release,
  and uncached fallback when a pinned singleton cache cannot admit another
  expert.
- `Makefile` links `src/expert_cache.c` into `test-maple-expert-store`.

### Verified commands

These passed after adding the shared pinning layer and Maple lease layer:

```sh
cc -O1 -Isrc tests/test_expert_cache.c src/expert_cache.c \
  -o /private/tmp/test_expert_cache_pin && /private/tmp/test_expert_cache_pin

make test-maple-expert-store
```

### Packed-reader follow-up

After the passing `make test-maple-expert-store` run, the packed
`maple-experts.bin` reader in `src/maple/maple_expert_store.cpp` was changed
so `open_packed()` exposes the same six per-expert regions as
`open_direct_safetensors()` instead of treating a packed record as one opaque
region. This is needed for MLX no-copy projection views in M3 and is covered
by the current store and view targets.

The baseline checks are:

```sh
make test-maple-expert-store
cc -O1 -Isrc tests/test_expert_cache.c src/expert_cache.c \
  -o /private/tmp/test_expert_cache_pin && /private/tmp/test_expert_cache_pin
```

### Previous implementation step (completed below)

Continue M3. Add MLX no-copy tensor views over a `MapleExpertLease`:

- gate packed weight: `{moe_intermediate_size, hidden_size / 16}`, `uint32`;
- gate row alpha: `{moe_intermediate_size}`, `bfloat16`;
- up packed weight: `{moe_intermediate_size, hidden_size / 16}`, `uint32`;
- up row alpha: `{moe_intermediate_size}`, `bfloat16`;
- down packed weight: `{hidden_size, moe_intermediate_size / 16}`, `uint32`;
- down row alpha: `{hidden_size}`, `bfloat16`.

The view deleter must keep the `MapleExpertLease` alive until MLX has finished
with the array data. The streamed one-expert path must force evaluation before
the lease is released.

After that, compare one streamed expert's gate/up/down quantized matmuls and
weighted contribution against the current resident `take()` implementation on
fixture-scale tensors before replacing full decode.

### Current implementation checkpoint — 2026-08-11 (M4/M5 parity complete)

The production Maple model now uses the same bounded policy as Qwen: resident
non-expert tensors stay loaded, routing selects expert IDs, and only those
packed expert records are read from SSD through the shared byte-budgeted LRU.
The loader fails closed if the packed manifest/store is missing or invalid.
The real checkpoint was never loaded fully resident during this verification.

The frozen-reference mismatch had two concrete MLX graph-shape causes:

1. The checkpoint decorates `clamped_swiglu` with
   `mx.compile(shapeless=True)`. The native implementation had emitted clamp,
   sigmoid, and multiply as separate primitives. That first changed one BF16
   value in layer 21 and the difference amplified through layers 22-23.
   Native streamed expert paths now use the equivalent compiled graph.
2. Native prefill projected only the last hidden row through `lm_head`; the
   reference projects the complete prompt chunk and then slices its final row.
   MLX chooses a different quantized-matmul kernel for one row versus multiple
   rows. The native path now projects the whole bounded chunk before slicing.

A bounded layer oracle established that embeddings and layers 0-20 were exact,
attention and norms in layer 21 were exact, and the first discrepancy was the
MoE activation. With compiled SwiGLU, all 24 hidden-state outputs became exact;
with the reference `lm_head` row shape, raw logits became bit-exact.

The final prefill path is chunked at 64 tokens. Within a chunk it sorts routes,
copies only the selected experts from SSD into a compact RHS tensor, evaluates
that layer, and releases the transient buffer. It never creates a 24-layer or
full-model expert tensor. The pathological one-layer maximum is all 256 experts
(about 193.5 MiB transient); decode needs only the selected top eight. The
resident checkpoint payload is 0.408 GiB and the production cache ceiling is
about 73.5 MiB.

Final target-Metal verification:

```text
Expert store: PASS
Expert MLX views, aggregate, lease lifetime, and pressure reclaim: PASS
Fused components: PASS
Attention fixture: max error 3.29018e-05; fused decode error 0; PASS
Raw real-checkpoint logits: max error 0; mean error 0; PASS
Frozen greedy catalogue: 10/10 prompts, 320/320 generated tokens;
  peak process-tree RSS 876 MiB, swap growth 0 MiB; PASS
Guarded real native run (64 MiB override):
  physical footprint after prefill 820 MiB, after decode 840 MiB
  peak process-tree RSS 687 MiB, swap growth 0 MiB; PASS
Production HTTP run (default cache):
  visible response "hello"; streaming=true
  budget/peak payload 77,070,336 bytes; 96 entries
  failed admissions 0; warning/critical pressure events 0; PASS
```

`tools/run_memory_guarded.sh` now reports peak process-tree RSS as well as swap
growth and forwards shutdown signals to the guarded child. The real lifecycle
test runs both self-test and HTTP server through this guard with default hard
limits of 1,000 MiB RSS and 64 MiB new swap.

Remaining work is release hardening rather than a Maple correctness blocker:
long-context ceiling measurement, cancellation/concurrency stress, and final
gateway/catalogue rollout checks. The production SSD-streamed native model,
raw-logit parity, greedy generation, HTTP serving, telemetry, and fail-closed
memory behavior are working.

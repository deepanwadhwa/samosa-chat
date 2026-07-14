# Thinking-mode diagnosis

## Current conclusion

The failures had multiple causes. Calling the whole problem "int4 collapse"
was too broad, but saying int4 was irrelevant was also too strong.

1. The first empty/aborted long runs were an engine stack overflow when GQA
   context exceeded 4,096 tokens. That bug is fixed separately.
2. Unfinished *reasoning* was primarily a sampling-profile error. The runner
   applied one generic profile to every task and omitted Qwen's general-mode
   presence penalty.
3. After the profile fix, long WebDev output could still enter a repetition
   attractor. The fast kernel is W4A8: in addition to int4 stored weights it
   re-quantizes every activation row to int8. Keeping the same int4 files but
   using float activations crossed the fast run's failure point without an
   attractor. It still produced isolated `of ofof` defects, so weight-only
   int4 is not lossless.
4. The stored rowwise-int4 format has substantial measured numerical error,
   but it is **not** established as the leading cause of every behavioral
   failure. Despite the old `Q4_0` label, it uses one symmetric scale for an
   entire output row (512 or 2,048 weights in the expert matrices), not a small
   block scale. Direct BF16 range samples quantify that damage, but generation
   attribution requires a matched upstream control and a non-confounded token
   budget. The original group-32 arithmetic gate lacked both.

The routed/shared MoE down projection is the narrowest *activation-precision*
boundary that stabilized the recorded WebDev prompt. Its input is the
high-dynamic-range `silu(gate) * up` intermediate. Keeping that input in float
while leaving the rest of the accelerated path enabled completed the requested
HTML without an attractor and retained useful speed. This is a containment
measure for that profile, not a general correction for the stored rowwise-q4
weights.

## Controlled experiment ledger

This ledger is directional evidence about failure mechanisms, not a release
pass/fail result for any profile. Most cells are one prompt and one seed, and
the three-token prompt variant below already proves that such a cell is not
predictive of robustness. Release claims require prompt variants and multiple
seeds.

All WebDev controls below use the same landing-page prompt, int4 container,
seed 7, tokenizer, and four OpenMP threads unless the row says otherwise.
"Complete" requires both structural thinking closure and the task-specific
`</html>` marker; merely producing text after `</think>` is not completion.

| Run | Activation path | Sampling | Limit / generated | Result | Decode |
|---|---|---|---:|---|---:|
| Original WebDev | W4A8 fast | temp 1.0, top-p .95, presence 0 | 8,192 / 8,192 | never closed `</think>`; repetitive reasoning tail | recorded prior run |
| Official WebDev short control | W4A8 fast | temp .6, top-p .95, top-k 20, presence 0 | 4,096 / 4,096 | closed thinking; coherent HTML/CSS, but hit cap before task completion | 8.54 tok/s |
| Official WebDev long control | W4A8 fast | same | 8,192 / 8,192 | closed thinking, then repeated `<p class="tagline">` hundreds of times | 6.57 tok/s |
| Sampler stress | W4A8 fast | official + presence .2 | interrupted after loop appeared | repeated `.flex-evenly` rule 34+ times; penalty did not fix it | not retained |
| Full float validation | W4 + float activations | official | 3,000 / 3,000 | clean and structured; cap reached | 4.34 tok/s |
| Full float extended | W4 + float activations | official | 5,000 / 5,000 | crossed fast failure point without loop; still incomplete at cap; isolated `of ofof` defects | 4.19 tok/s |
| DeltaNet-only float | W4A8 except DeltaNet projections | official | 335 / 335 | rejected: emitted end-of-turn inside reasoning without `</think>` | 8.03 tok/s |
| MoE-down-only float, short | W4A8 except routed/shared expert down projections | official | 1,000 / 1,000 | closed thinking and began coherent HTML; cap reached | 9.62 tok/s |
| MoE-down-only float, extended | same | official | 5,000 / 5,000 | crossed old failure point; complete `</html>` before cap; no attractor; minor text defects remain | 6.47 tok/s |

General-reasoning control, seed 11: the original temp 1.0 / presence 0 run
looped to 8,192 tokens without closing `</think>`. With Qwen's general profile
(temp 1.0, top-p .95, top-k 20, presence 1.5), the same int4 runtime closed
thinking, returned the correct C unsigned-loop diagnosis and fix, and emitted
end-of-turn after 1,407 tokens at 8.91 tok/s.

Applying the WebDev MoE-down selective boundary to that general prompt was
rejected: although it recognized unsigned underflow, its reasoning repeatedly
proposed `++i` in the reverse loop instead of `--i` and had not closed
`</think>` when stopped. Selective precision is therefore scoped to the
`think-code` profile.

A later three-token prompt variant exposed that the apparently successful
general control was not robust: it closed after 262 tokens but returned an
incomplete, incorrect fix. This is deterministic prompt sensitivity, not
hardware nondeterminism; rerunning the exact same configuration and seed gives
a byte-identical prefix.

### Resident-int8 isolation (2026-07-14)

The official checkpoint was streamed one shard at a time to build a hybrid
with int8 embeddings, LM head, attention, and other resident matrices while
hard-linking the exact same rowwise-int4 expert file. Conversion peaked around
5 GB RSS, used 5 MB swap, and enforced a 10 GiB free-disk stop floor.

On the original C unsigned-loop prompt, seed 11, official general profile and
fast activations, the hybrid stayed coherent but used all 1,600 tokens without
closing `</think>` or providing fixed code. It correctly found both unsigned
failure modes, so resident int8 improved neither completion reliability nor
conciseness enough to pass the product gate. The run decoded at 7.28 tok/s,
peaked at 3.59 GB RSS, reported no cache pressure, and reread 447.41 GB of
expert data. A direct-mode control was also correct in its explanation but hit
a 400-token cap immediately before the requested code. These controls rule out
resident rowwise-int4 as the sole cause and point back to the much larger expert
store and unbounded reasoning trajectory.

### Group-32 artifact baseline (2026-07-14)

The pinned official checkpoint was fully reconverted to group-32 symmetric q4
experts plus resident row-q8. The artifact contains 10,240 regular experts at
1,966,080 bytes and 256 MTP experts at 3,162,112 bytes. Its manifest records
`groupwise-symmetric-q4-v1`, group size 32, and densely covers the
20,942,159,872-byte expert store. The runtime loaded the new format and passed
a one-token two-core smoke at 3.88 GB peak RSS.

A bounded direct-mode control on the unsigned C reverse-loop task was coherent
and correct. It emitted model end-of-turn after 193 tokens, including the safe
`for (size_t i = n; i-- > 0;)` form, at 7.27 tok/s and 3.85 GB peak RSS. It read
68.38 GB of experts and avoided 61.41 GB through cache hits.

A bounded general-thinking arithmetic control, seed 11, originally appeared
to fail. The reasoning reached the correct `7 red, 4 blue, 11 total` result,
but did not close naturally within the 256-token thinking budget. The forced
transition occurred and the post-`</think>` answer ended incomplete. The run
generated 272 tokens at 6.54 tok/s, peaked at 3.85 GB RSS, and read 114.94 GB
of experts.

That interpretation is superseded: six upstream FP8 controls used 353--616
reasoning tokens and 587--928 completion tokens, so the 256/512 gate was
undersized on both dimensions. With a 1,024 thinking budget and 2,048 outer
ceiling, the same local group-32 prompt/seed closed naturally, answered
correctly, and stopped after 933 tokens. The old cell is not evidence of
quantization instability. See `UPSTREAM_CONTROL_2026-07-14.md`.

Conversion stayed within the machine-safety gates. A finalization defect was
also removed: the source shards append layers in a valid but non-numeric order,
which previously triggered a redundant full-store repack with no disk-floor
check. Dense validated append stores are now published directly because runtime
lookup is manifest-driven, avoiding an unnecessary approximately 20 GB write.

### Source-weight quantization check

To avoid another 72 GB download, byte ranges were read directly from upstream
revision `995ad96eacd98c81ed38be0c5b274b04031597b0`. The gate/up sample is
bytes 1,084,230,432–1,084,754,719 of shard 1; the down sample is bytes
371,536–502,607 of shard 2 (ranges include each safetensors header offset).
BF16 values were expanded exactly by shifting their 16-bit payload into the
high half of float32. The calculation compared the current whole-row
symmetric q4, symmetric q4 with 32-weight groups, and whole-row q8 on the same
source values.

| BF16 sample | Current row-q4 NRMSE / zeros | Group-32 q4 NRMSE / zeros | Row-q8 NRMSE / zeros |
|---|---:|---:|---:|
| layer 0 gate/up, 128 rows x 2,048 | 19.31% / 72.50% | 10.64% / 68.09% | 1.07% / 63.06% |
| layer 0 down, 128 rows x 512 | 12.62% / 62.41% | 8.40% / 59.84% | 0.70% / 55.10% |
| layer 20 gate/up, 128 rows x 2,048 | 15.38% / 21.04% | 9.76% / 13.46% | 0.85% / 1.19% |
| layer 20 down, 128 rows x 512 | 13.62% / 18.17% | 9.76% / 13.01% | 0.75% / 0.99% |
| layer 39 gate/up, 128 rows x 2,048 | 19.58% / 23.15% | 10.34% / 13.79% | 1.09% / 1.31% |
| layer 39 down, 128 rows x 512 | 13.98% / 19.72% | 9.98% / 14.10% | 0.77% / 1.09% |

The zero fraction includes genuinely tiny source values, so it is not an error
metric by itself. NRMSE is normalized by source RMS and directly shows that
one scale across a full row discards substantially more signal than groupwise
q4. This matches the behavioral A/B results: float activations can prevent one
attractor, but cannot recover information already removed from the weights.

Layer 0 has an atypical zero distribution, but the mid and late samples retain
the same NRMSE ordering, so the conclusion is not an artifact of sampling only
the first layer. Group-16 q4 reduced sampled mid/late NRMSE further to
8.56--8.84%, but group-32 still leaves roughly 10% error and is only an
experimental baseline. It must not be called a release format before behavioral
parity tests across upstream-calibrated prompt families. NRMSE establishes a
format-quality concern; it does not by itself establish a generation failure.

Storage and read amplification constrain the next choice. An aligned group-16
q4 expert blob is about the same size as group-32 q4 gate/up with row-q8 down
(about 2.36--2.38 MB per regular expert). The mixed candidate nearly eliminates
the sampled down-projection error while group-16 improves every projection.
Both therefore need behavioral and I/O comparison; NRMSE alone does not select
the winner. The corrected group-32 control removes the urgency to build either
artifact before a broader local/upstream parity gap is demonstrated.

### Activation-aware candidate

The existing candidates change only round-to-nearest quantization geometry.
AWQ instead uses offline activation statistics and equivalent channel scaling
to protect salient channels without mixed-precision storage. For this MoE MLP,
an experimental equivalent transform can scale an `up` output row and
inversely scale the matching `down` input column before q4 quantization. That
preserves the unquantized function while changing where q4 error lands, with no
new runtime tensor if the scales are folded into the weights.

This is strategically preferable to q8-down if it reaches parity because it
does not impose the mixed artifact's 20.83% expert-size increase. It is not yet
ready to implement blindly: the current teacher stream captures logits, not
per-expert `silu(gate) * up` channel statistics. A valid experiment needs a
representative BF16 activation-statistics hook, adequate sparse-expert
coverage, and a same-size A/B against plain group-32.

- AWQ: <https://arxiv.org/abs/2306.00978>
- SmoothQuant equivalent scaling: <https://arxiv.org/abs/2211.10438>

### Rotation-based candidates

The 2025 TurboQuant and PolarQuant papers evaluate online/KV-cache
quantization. They may become useful for long contexts, but they do not replace
the damaged stored expert weights diagnosed here. This model has only ten
full-attention layers and two KV heads, so its current float32 KV cache is about
40 KiB/token (roughly 320 MiB at the 8,192-token product cap); the expert store
is the larger immediate constraint. A separate 2026 paper named PolarQuant
proposes Hadamard-rotated weight quantization and is directly relevant in
principle. That paper is currently withdrawn with the author's note that errors
need fixing, so it is an experimental candidate rather than a release
dependency. Any rotation-based prototype must beat group-16 q4 and the mixed
q8-down candidate on correctness, expert bytes read, cache behavior, RSS,
speed, swap, and thermals on this machine.

- TurboQuant: <https://arxiv.org/abs/2504.19874>
- KV-cache PolarQuant: <https://arxiv.org/abs/2502.02617>
- Withdrawn weight PolarQuant: <https://arxiv.org/abs/2603.29078>

## Why the earlier benchmark appeared healthy

The frozen quality harness accepted expected substrings anywhere in generated
text. Across each of five recorded full suites, 14/15 thinking prompts passed
their string checks even though 0/15 emitted `</think>`. Expected phrases such
as `Answer: 72` were mentioned inside unfinished reasoning and counted as final
answers.

`tools/check_thinking_output.py` now checks:

1. a closing `</think>` marker;
2. a non-empty final answer after that marker;
3. global and tail repeated four-gram ratios;
4. consecutive repeated-line runs; and
5. optional task-specific completion markers via repeatable `--require TEXT`.

For the WebDev regression, use `--require '</html>'`. The generic checker alone
correctly detects degeneration but cannot infer that a particular requested
artifact is unfinished.

## Product sampling profiles

- General thinking: temperature 1.0, top-p .95, top-k 20, presence 1.5.
- Precise coding/WebDev: temperature .6, top-p .95, top-k 20, presence 0.
- Direct mode: temperature .7, top-p .8, top-k 20, presence 1.5.

General and direct profiles currently use the accelerated activation path.
Precise coding/WebDev currently uses float input for routed/shared expert down
projections. None of these profiles should be called release-stable until
structural and correctness rates reach a declared parity threshold against an
upstream-calibrated control across prompt variants and repeated runs. A new
expert format is required only if a matched gap remains.

These defaults follow the Qwen3.6 model card. Explicit engine flags still
override individual sampler values. Correct sampling fixes the reasoning-phase
failure; it does not by itself certify a long final answer on the W4A8 path.

## Token ceilings

The token option is a maximum, not a fixed answer length. Generation stops
early when the model emits end-of-turn/end-of-text. Otherwise it stops at the
configured ceiling and is incomplete. All product profiles now default to at
most 8,192 new tokens; `--max-tokens N` overrides that outer ceiling.

Thinking has a second, smaller safety bound. General reasoning may use up to
1,024 internal tokens and precise code up to 2,048. The model may emit
`</think>` earlier; if it does not, the engine appends Qwen's published
natural-language early-stop transition followed by `</think>`, then lets the
model produce its final answer with the remaining outer budget. The previous
bare-token injection did not match Qwen's trained protocol. The
`--thinking-budget N` override remains available. Forced closure is a safety
mechanism, not evidence of correctness. Release tests must separately report
natural versus forced thinking closure, structural completion, answer
correctness, repetition, and model-finished versus ceiling-reached.

## Machine-safety observations

The successful 5,000-token selective run used four performance threads only
for a controlled speed comparison. It peaked at 2.48 GB RSS, left 69% system
memory free, held swap at 5 MB, and produced no macOS thermal or performance
warning. The engine itself wrote no model data.

Expert-cache read amplification is substantial: this run cumulatively read
1.48 TB from the 17 GB expert store because evicted experts are reread. Reads
do not consume NAND program/erase cycles like writes, but sustained I/O costs
power and controller heat. Product defaults remain the cooler two-core mode;
future performance work should reduce read amplification without increasing
swap or memory pressure. Weight precision and read amplification are one
coupled decision: larger expert blobs increase bytes per miss and reduce the
number of expert identities that fit in a fixed-size cache. Every format
candidate must therefore report quality, blob size, cache hits, expert bytes
read, RSS, decode speed, swap delta, and thermal state together. The repetition
guard also prevents a bad attractor from burning hours of CPU and repeated SSD
reads.

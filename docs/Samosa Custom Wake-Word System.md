# Samosa Custom Wake-Word System
## Implementation Specification — Method A First, Method B Second

## 1. Objective

Implement a fully local custom wake-word system for Samosa with this target user experience:

```text
Samosa:
Choose your wake word.

Say it now.
[User says: "Mirchi"]

Great. Say it again.
[User says: "Mirchi"]

One more time.
[User says: "Mirchi"]

Wake word ready.
```

After enrollment:

```text
User: "Mirchi"

Samosa wakes immediately.
```

The user must **never** need to:

- train a model;
- create an ONNX file;
- understand machine learning;
- specify phonemes;
- download a wake-word model manually;
- upload samples to a server;
- use Python;
- use a cloud service.

Everything must work locally.

The implementation has two independent approaches:

**Method A — PRIMARY:** few-shot acoustic enrollment using speech embeddings + DTW.

**Method B — SECONDARY:** sherpa-onnx open-vocabulary keyword spotting.

**The agent MUST implement and validate Method A before doing any work on Method B.**

---

# 2. Why We Are Not Using the Existing `computer-use-agent` Approach

The referenced `computer-use-agent` uses openWakeWord for its offline wake-word implementation. Arbitrary offline wake words require a separately trained `.onnx` model. Its alternative arbitrary-phrase mode uses STT instead and does not provide the same wake-word barge-in behavior.

That is explicitly **not** the Samosa UX we want.

Do not implement:

```text
record wake word
→ train classifier
→ produce mirchi.onnx
→ load mirchi.onnx
```

Do not launch openWakeWord training.

Do not run a Colab.

Do not generate synthetic training data.

Do not build a wake-word classifier.

---

# 3. Required Overall Architecture

Expose one common Samosa wake-word abstraction:

```text
                 ┌── Method A: acoustic templates
microphone ──────┤
                 └── Method B: sherpa KWS
                         │
                         ▼
                  wake_event()
                         │
                         ▼
             existing Samosa voice system
```

The rest of Samosa must not care which backend generated the event.

Suggested logical interface:

```text
wake_init()
wake_start()
wake_stop()
wake_is_running()

wake_enroll_begin()
wake_enroll_add_sample()
wake_enroll_finish()

wake_process_audio(samples, count)

wake_set_threshold(...)
wake_get_status(...)

callback:
    on_wake_detected(...)
```

Names can be adapted to existing Samosa conventions.

Do **not** create a second microphone subsystem if Samosa already has audio capture. Reuse the existing microphone/audio ownership architecture wherever possible.

The wake detector should receive PCM samples from the same audio pipeline.

---

# 4. Mandatory Machine-Safety Constraints

Development is happening on a **16-GB unified-memory Mac**. The wake-word feature itself is tiny and there is absolutely no justification for stressing this machine.

These are hard requirements.

## Build safety

Never run:

```text
make -j
make -j$(nproc)
cmake --build . -j$(...)
pytest -n auto
```

Use at most:

```text
-j2
```

or:

```text
CMAKE_BUILD_PARALLEL_LEVEL=2
```

for this work.

If Samosa already has a normal safe build path, use it rather than rebuilding unrelated components.

## Runtime safety

Wake-word inference should use:

```text
CPU threads: 1
```

unless benchmarks demonstrate an actual reason to use more.

The local-wake reference itself explicitly configures ONNX Runtime with one intra-op thread.

Do not launch:

- the main LLM during isolated wake-word benchmarks;
- multiple models simultaneously;
- Docker solely for this work;
- GPU stress tests;
- parallel benchmark workers;
- large synthetic audio generation jobs;
- large datasets;
- model training.

## Memory targets

Target incremental memory consumption for the wake subsystem:

```text
Preferred:   < 200 MB RSS
Acceptable:  < 512 MB RSS
Unacceptable: > 1 GB
```

If the isolated wake-word process crosses approximately 1 GB RSS, stop and investigate. A tiny wake-word detector consuming gigabytes indicates a bug or inappropriate dependency.

The Method A embedding model in the reference implementation is only about **2.32 MB**.

Method B's INT8 English sherpa KWS neural files are also very small: approximately 4.6 MB encoder + 272 KB decoder + 160 KB joiner.

## Benchmark safety

All automated performance tests must stream audio.

Never load hours of audio into RAM.

Process:

```text
file → small PCM chunks → wake detector
```

Do not construct giant arrays containing an entire negative corpus.

A normal development benchmark should terminate within a few minutes.

Long-duration false-positive testing should run approximately in real time or otherwise remain bounded to roughly one CPU core.

---

# 5. METHOD A — PRIMARY IMPLEMENTATION

## 5.1 Concept

Method A is based on `local-wake`.

`local-wake` supports arbitrary user-defined wake words without training a new wake-word classifier. It extracts neural speech features and performs time-warped comparison against user-recorded reference examples. The project recommends approximately **3–4 reference recordings**.

The architecture is:

```text
ENROLLMENT

"Mirchi" recording 1
        ↓
speech embedding
        ↓
template 1

"Mirchi" recording 2
        ↓
speech embedding
        ↓
template 2

"Mirchi" recording 3
        ↓
speech embedding
        ↓
template 3
```

Runtime:

```text
microphone
   ↓
rolling audio window
   ↓
speech embedding
   ↓
cosine DTW against templates
   ↓
distance score(s)
   ↓
threshold / consensus
   ↓
WAKE
```

There is **no per-word model training**.

The wake word therefore does not need to be an English dictionary word.

Required human test words include:

```text
Mirchi
Rekha
Samosa
Hey Samosa
Zavora      # deliberately invented word
```

Do not claim language independence from theory alone. Verify these words experimentally.

---

# 5.2 Reference Algorithm That Must First Be Reproduced

Before attempting improvements, reproduce the core local-wake algorithm accurately.

The reference implementation uses:

```text
sample rate:        16,000 Hz
channels:           mono
embedding backend:  speech-embedding ONNX
ONNX threads:       1
rolling buffer:     2.0 seconds
sliding step:       0.25 seconds
distance:           cosine DTW
```

The documented default rolling buffer and slide are 2.0 s and 0.25 s respectively. The documentation specifically notes that a smaller slide increases precision at additional CPU cost.

The reference embedding path takes audio samples, feeds them through `speech-embedding.onnx`, extracts the embedding sequence, and compares sequences with Dynamic Time Warping using cosine distance.

The reference runtime maintains a 2-second rolling buffer, advances it one slide at a time, extracts features and compares the result against each stored reference template.

### Important

Do not immediately invent a different neural architecture.

First establish:

```text
Samosa implementation ≈ local-wake reference
```

Then optimize.

---

# 5.3 Model Asset

Method A requires one **generic speech-embedding model**, not one model per wake word.

Reference model:

```text
speech-embedding.onnx
~2.32 MB
```



The user must never manage this file.

From the user's perspective:

```text
Samosa contains wake-word capability.
```

not:

```text
Please place an ONNX file in ~/.samosa/models/wake/
```

The asset should either:

1. ship as part of Samosa, or
2. be automatically installed as an internal Samosa component.

### Licensing gate

`local-wake` itself is MIT licensed and identifies its feature model as Google's `speech-embedding` converted to ONNX.

**Before redistributing the ONNX inside Samosa, verify the upstream model/artifact license.**

Do not assume:

```text
local-wake code is MIT
therefore every bundled model has identical redistribution terms
```

Document the model origin, license and SHA-256 in Samosa's model manifest.

This license verification is a release requirement, not an excuse to block the prototype.

---

# 5.4 Production Runtime

Do not ship the Python `local-wake` package.

Its current Python dependencies include ONNX Runtime, Silero VAD, librosa, sounddevice, soundfile and NumPy.

Those are useful references, not the desired Samosa production stack.

Production implementation should use Samosa's native architecture.

Preferred:

```text
C/C++ Samosa code
     ↓
ONNX Runtime native API
     ↓
speech-embedding.onnx
```

Implement DTW natively.

Implement the ring buffer natively.

Reuse Samosa's existing audio capture.

Python may be used **only as a development reference oracle or one-time fixture generator**. No Python runtime may become necessary for wake-word operation.

---

# 5.5 Enrollment Pipeline

Implement:

```text
samosa wake enroll
```

or the equivalent command/UI appropriate to the existing project.

Default enrollment count:

```text
3 recordings
```

After three:

```text
Wake word ready.

Optional:
"Add one more recording for improved robustness?"
```

A fourth sample may be collected.

Do **not** make six recordings mandatory.

The local-wake documentation says 3–4 samples are usually sufficient; its published benchmark uses three reference recordings.

Six samples should be tested experimentally later.

### Recording procedure

For every reference sample:

1. Open microphone.
2. Capture approximately 3 seconds maximum.
3. Detect actual speech boundaries.
4. Trim leading/trailing silence.
5. Ensure the word was not clipped.
6. Normalize/convert to the format expected by the embedding model.
7. Extract embedding.
8. Save embedding template.

Reference local-wake enrollment records at 16 kHz mono and trims speech using VAD. It warns that aggressive VAD can accidentally trim part of the wake word.

Therefore Samosa must reject an obviously bad enrollment rather than silently accepting it.

Bad enrollment conditions include:

```text
no speech detected
recording too short
recording clipped
extreme microphone clipping
very low level
word appears cut off
```

User-facing behavior:

```text
I didn't capture that clearly. Please say it again.
```

Do not count failed captures toward 1/3, 2/3, etc.

---

# 5.6 VAD / Silence Trimming

First preference:

**reuse whatever VAD/audio segmentation Samosa already uses.**

Do not add Silero merely because local-wake happens to use Silero unless it is actually necessary.

The enrollment requirement is simply:

```text
wake-word speech must be isolated without clipping it
```

A lightweight energy/RMS-based trim may be sufficient.

Whichever implementation is chosen must have tests for:

```text
0.5 s silence + wake word + 0.5 s silence
quiet wake word
loud wake word
wake word beginning immediately
wake word ending near recording boundary
```

If Samosa's existing VAD works, use it.

---

# 5.7 Template Storage

Do not require retaining raw voice recordings permanently.

Preferred production representation:

```text
~/.samosa/...
    wake-profile
        metadata
        template-1
        template-2
        template-3
```

Store:

```text
backend version
embedding-model SHA
sample rate
embedding dimensions
reference embeddings
selected threshold
aggregation mode
enrollment timestamp/version
```

Raw WAV files should be:

```text
deleted after enrollment by default
```

unless required for debugging.

Development builds may support:

```text
--keep-enrollment-audio
```

but this must be opt-in.

If the embedding model changes incompatibly, detect the version mismatch and ask the user to re-enroll rather than interpreting incompatible templates.

---

# 5.8 Runtime Audio Processing

Baseline implementation:

```text
16-kHz mono PCM
     ↓
2.0-s ring buffer
     ↓ every 250 ms
embedding extraction
     ↓
DTW against enrolled templates
     ↓
wake decision
```

Do not allocate a new multi-megabyte buffer every 250 ms.

Use a fixed ring buffer or reusable memory.

Do not repeatedly reload the ONNX model.

Load one inference session when wake detection starts and reuse it.

The reference local-wake model is cached and its ONNX Runtime session uses one CPU execution thread.

After successful detection:

```text
emit wake_event
clear/reset rolling detector state
apply short refractory/cooldown period
```

This prevents the same utterance from triggering several times.

The reference implementation clears its rolling audio buffer after detection for the same reason.

---

# 5.9 DTW Implementation

Implement cosine-distance Dynamic Time Warping.

Reference behavior:

```text
embedding sequence A
embedding sequence B

local cost:
    cosine distance(frameA, frameB)

DTW:
    normal monotonic path through cost matrix

final score:
    accumulated path cost normalized by sequence length
```

The local-wake implementation computes a normalized DTW cosine distance and treats **lower distance as more similar**.

Write unit tests before relying on this code.

Tests must include:

```text
identical feature sequences
slightly time-stretched sequence
shortened sequence
completely different sequence
all-zero / degenerate vectors
very short sequence
```

No NaNs.

No division by zero.

No out-of-bounds behavior.

---

# 5.10 Wake Decision Logic

Implement two modes for evaluation.

### Mode 1 — reference-compatible

```text
distance against template 1
distance against template 2
distance against template 3

if ANY distance < threshold:
    trigger
```

This mirrors the basic local-wake reference behavior.

### Mode 2 — consensus

Evaluate:

```text
at least 2 templates must score below threshold
```

or equivalently evaluate the second-best distance.

Example:

```text
template distances:

0.08
0.10
0.31

threshold = 0.15

2 references agree → wake
```

Do not automatically assume Mode 2 is superior.

It may reduce false positives but could increase false negatives.

**Benchmark both and let measured results decide the production default.**

---

# 5.11 Threshold Calibration

Do not blindly copy one threshold from local-wake.

The documentation explicitly says thresholds may require adjustment for microphone/environment differences.

The published benchmark found approximately 0.1623 as its clean same-speaker optimum, but that number came from that particular dataset and experiment.

Therefore:

### Initial prototype

Expose:

```text
--threshold
```

for developers.

Test around a sensible range such as:

```text
0.08
0.10
0.12
0.14
0.16
0.18
0.20
```

Do not hardcode these as universal truths.

### Human calibration tool

Implement a developer/user test mode:

```text
samosa wake test
```

It should display detection scores.

Example:

```text
Listening...

Mirchi
score: 0.081   DETECT

Mirchi
score: 0.096   DETECT

Murky
score: 0.194   reject

random speech
score: 0.281   reject
```

This makes threshold problems diagnosable.

Eventually choose an automatic/default threshold based on empirical testing.

---

# 5.12 Enrollment-Count Experiment

The agent MUST specifically test whether additional enrollment samples improve performance.

Test:

```text
1 sample
2 samples
3 samples
4 samples
6 samples
```

Use the **same held-out recordings** for every comparison.

Do not compare:

```text
3 samples on easy data
vs
6 samples on different/harder data
```

For each enrollment count measure:

```text
true wake detections
missed wakes
false detections
negative rejections
average detection latency
CPU use
```

The local-wake repository exposes `REFERENCE_SET_SIZE` and uses three by default, but does not publish a 1-vs-2-vs-3-vs-4-vs-6 comparison.

Therefore this experiment must answer that question for Samosa.

Expected decision:

```text
If 4 materially improves 3:
    consider 4 as optional/recommended.

If 6 offers negligible benefit:
    don't inconvenience users with 6 recordings.
```

Do not manipulate the results to justify a preconceived answer.

---

# 6. METHOD A AUTOMATED TESTS

## 6.1 Unit tests

Required:

```text
wake_ring_buffer
wake_cosine_distance
wake_dtw
wake_template_serialization
wake_template_loading
wake_threshold_logic
wake_consensus_logic
wake_cooldown
wake_audio_resampling if applicable
```

---

# 6.2 Reference parity test

Use local-wake only as a development oracle.

Create several small fixed WAV fixtures.

Run the Python reference once and record:

```text
embedding dimensions
DTW distances
expected ordering of similarity
```

Then verify native Samosa produces numerically equivalent or sufficiently close outputs.

Once fixtures are generated, normal Samosa tests must **not depend on Python**.

---

# 6.3 Audio fixture tests

Create a tiny test set.

Examples:

```text
wake_positive_01.wav
wake_positive_02.wav

negative_speech_01.wav
negative_speech_02.wav

silence.wav
noise.wav
```

Keep fixtures small.

Do not commit huge corpora.

---

# 6.4 Performance test

Measure:

```text
RSS before wake detector
RSS after model load
RSS while running

CPU usage while listening
average inference time
p95 inference time

detection latency
```

Target:

```text
processing comfortably faster than real time
one inference thread
no continuously growing memory
```

Perform a leak test:

```text
run detector repeatedly / stream several minutes
measure RSS periodically
```

RSS must stabilize.

---

# 7. METHOD A — MANDATORY HUMAN ACCEPTANCE TEST

**This is a hard release gate. Automated tests alone are insufficient.**

The agent must expose a simple human-test workflow.

Example:

```text
$ samosa wake enroll

Choose a wake word.
You do not need to type it.

Recording 1 of 3...
✓

Recording 2 of 3...
✓

Recording 3 of 3...
✓

Wake word enrolled.

Now testing.
Say your wake word whenever you want.
Press Ctrl-C to finish.
```

The human must then test real microphone behavior.

## Test set 1 — Mirchi

Enroll:

```text
Mirchi × 3
```

Then say `"Mirchi"` naturally at least 10 times.

Vary:

```text
normal voice
quiet voice
slightly louder
faster
slower
different microphone distance
```

Record:

```text
detected / attempted
false triggers
latency
```

---

## Test set 2 — Rekha

Repeat independently:

```text
Rekha × 3
```

Same test.

---

## Test set 3 — Samosa

```text
Samosa × 3
```

Same test.

---

## Test set 4 — phrase

```text
Hey Samosa × 3
```

Same test.

---

## Test set 5 — invented word

Example:

```text
Zavora × 3
```

This tests whether the implementation is actually acoustically matching rather than accidentally relying on conventional English vocabulary.

---

# 7.1 Negative Human Tests

For every wake word, speak similar but incorrect phrases.

For `"Mirchi"` test examples such as:

```text
mercy
merch
murky
chilli
Mickey
```

Also speak ordinary sentences for several minutes.

Play normal room audio if convenient:

```text
conversation
podcast
TV
music
```

Record false triggers.

A wake detector that recognizes `"Mirchi"` 10/10 but triggers every few minutes during normal conversation is not acceptable.

---

# 7.2 Human Test Report

The agent must produce something similar to:

```text
CUSTOM WAKE WORD HUMAN TEST

Backend: A
Enrollment samples: 3
Threshold: 0.14
Decision mode: consensus

Wake word: Mirchi
Positive trials: 10
Detected: 10
Missed: 0

Near-negative trials: 10
False wakes: 0

Wake word: Rekha
Positive trials: 10
Detected: 9
Missed: 1

...

RSS:
idle baseline:
wake detector:

CPU:
average:

Conclusion:
PASS / FAIL
```

Do not simply write:

```text
"It seems to work."
```

Provide numbers.

---

# 8. METHOD A ACCEPTANCE GATE

Method A is complete only when all of the following are true:

```text
[ ] arbitrary wake-word enrollment works
[ ] exactly 3 samples are sufficient for basic use
[ ] optional fourth sample supported
[ ] no wake-word-specific model is trained
[ ] no user-provided ONNX required
[ ] no cloud required
[ ] Mirchi works
[ ] Rekha works
[ ] Samosa works
[ ] invented word tested
[ ] multi-word wake phrase tested
[ ] negative phrases tested
[ ] threshold can be inspected/tuned
[ ] enrollment templates persist across restart
[ ] wake event reaches Samosa voice stack
[ ] repeated trigger/cooldown works
[ ] memory stays bounded
[ ] CPU use is reasonable
[ ] human microphone test completed
[ ] human test report produced
```

**STOP HERE if these requirements are not satisfied.**

Do not begin Method B merely because Method A became inconvenient.

Fix A first.

---

# 9. METHOD B — SHERPA-ONNX OPEN-VOCABULARY KWS

Method B answers a different question:

> Can one tiny generic KWS model recognize a user-selected textual keyword without per-user acoustic enrollment?

Sherpa-onnx describes its open-vocabulary keyword spotter as a tiny ASR-like system restricted to configured keywords. Keywords can be changed without retraining the underlying model.

Sherpa-onnx supports C and C++ and supports macOS arm64, making native Samosa integration viable.

This method is useful as an alternative backend, **not as a replacement for A until tested.**

---

# 9.1 Language Limitation

Do not claim sherpa KWS is universally multilingual.

Current documented KWS models include:

```text
Chinese + English 3M
Chinese 3.3M
English 3.3M
```



The English GigaSpeech 3.3M model explicitly supports only English.

The newer 3M model explicitly supports Chinese + English and maps English words through an English phone lexicon.

Therefore:

```text
Mirchi
Rekha
```

must be treated as experiments under Method B.

They might work through available English phone/token representations.

They might work poorly.

They might require pronunciation/token manipulation.

**Do not fake success.**

If B cannot reliably detect them, report:

```text
Method B cannot reliably support these wake words with the tested model.
```

That is an acceptable experimental result.

---

# 9.2 Model Choice

Start with an INT8 sherpa KWS model.

Do not start with a large ASR model.

For the English GigaSpeech KWS model, documented INT8 components are approximately:

```text
encoder  4.6 MB
decoder  272 KB
joiner   160 KB
```



Sherpa's documented example itself uses:

```text
num_threads = 1
provider = CPU
```



Follow that initially.

---

# 9.3 Method B Architecture

```text
user enters/selects wake phrase
        ↓
keyword → supported tokens / phones
        ↓
sherpa KWS keyword configuration
        ↓
continuous microphone stream
        ↓
tiny streaming Zipformer
        ↓
keyword trigger
        ↓
wake_event()
```

No per-user neural training.

---

# 9.4 Method B User UX

For B, a text representation is necessary.

Example:

```text
Choose wake word:
Mirchi
```

Samosa generates whatever keyword representation sherpa requires internally.

The user must never manually write:

```text
M IH1 R CH IY0 ...
```

Any phone/token conversion must be internal.

If automatic conversion cannot represent the requested word reliably:

```text
This wake word is not supported well by the current keyword model.
Try acoustic enrollment instead.
```

Then fall back to Method A.

---

# 9.5 Method B Tests

Use the exact same human wake-word suite as A:

```text
Mirchi
Rekha
Samosa
Hey Samosa
Zavora
```

This gives an apples-to-apples comparison.

Measure:

```text
positive recall
false positives
false negatives
detection latency
RSS
CPU
setup complexity
language limitations
```

---

# 10. FINAL A vs B COMPARISON

After both implementations exist, produce:

```text
                  Method A       Method B
------------------------------------------------
Requires training      no             no
User recordings        3–4            no
Text spelling          no             yes
Unknown words          ?              ?
Hindi-derived words    ?              ?
Same-speaker recall    ?
Cross-speaker recall   ?
False wakes/hour       ?
CPU                     ?
RAM                     ?
Latency                 ?
Model size              ?
```

Fill the question marks with measured data.

Do not fill them with guesses.

---

# 11. Likely Product Direction

Unless testing proves otherwise, Samosa should conceptually treat:

```text
Method A
= personalized custom wake word

Method B
= text-configured generic wake word
```

Method A has an important UX advantage:

```text
User simply says the desired sound.
```

No language model, spelling, dictionary or pronunciation mapping is inherently required.

Method B has a different advantage:

```text
No enrollment recordings required.
```

But that comes with the phonetic/language coverage limitations of the underlying KWS model.

---

# 12. Integration With Samosa Voice / Barge-In

Wake detection must ultimately feed the same event system used by Samosa's hands-free voice interface.

Expected states:

```text
IDLE
    wake detector active

WAKE DETECTED
    ↓
STT listening

LLM PROCESSING
    detector behavior according to existing voice architecture

TTS SPEAKING
    wake detector active for barge-in

USER SAYS WAKE WORD
    ↓
stop TTS
    ↓
start STT
```

The linked `computer-use-agent` demonstrates this overall barge-in pattern by keeping wake detection active while TTS is speaking and using a separate higher barge-in threshold to reduce echo triggers.

Samosa should eventually evaluate the same idea.

However, do **not** let TTS/barging complexity block initial Method A validation.

First prove:

```text
idle → wake word → event
```

Then integrate barge-in.

---

# 13. Suggested Work Order

The agent should perform the work in exactly this order.

### A1 — Inspect existing Samosa audio architecture

Find:

```text
microphone capture
sample format
sample rate
STT/VAD ownership
voice state machine
existing callbacks/events
```

Do not modify code yet.

### A2 — Build standalone native Method A comparison

Implement:

```text
embedding inference
cosine distance
DTW
template comparison
```

Validate against local-wake fixtures.

### A3 — Enrollment

Implement 3-sample enrollment.

### A4 — Live microphone detector

Implement ring buffer + 250-ms slide.

### A5 — Persistence

Save/load templates.

### A6 — Human custom-wake test

Test `"Mirchi"` first.

Then:

```text
Rekha
Samosa
Hey Samosa
invented word
```

### A7 — Threshold/decision evaluation

Compare:

```text
single-best template
2-template consensus
```

### A8 — Enrollment-size experiment

Compare:

```text
1
2
3
4
6
```

### A9 — Resource benchmark

Measure CPU/RSS/latency.

### A10 — Integrate with Samosa voice state machine

Prove:

```text
wake → STT
```

Then test TTS barge-in.

### A11 — Produce Method A report

Only when Method A passes, continue.

### B1 — Add sherpa-onnx KWS prototype

Use tiny INT8 model and one CPU thread.

### B2 — Test normal supported English phrase

Establish correct sherpa integration first.

### B3 — Test arbitrary words

Test:

```text
Mirchi
Rekha
Samosa
Zavora
```

### B4 — Compare with A

Produce final benchmark/report.

---

# 14. Explicit Things the Agent Must NOT Do

Do not:

```text
train openWakeWord
train another wake model
download multi-GB speech datasets
download Qualcomm dataset unless explicitly asked
start the Samosa LLM for isolated KWS tests
spawn all CPU cores
run giant parallel tests
run Docker just to test wake words
retain hours of microphone audio
load entire long WAVs into RAM
write a Python-only production implementation
require Python at runtime
require users to provide an ONNX model
hardcode "Mirchi" or "Rekha"
pretend B supports Hindi without testing
declare success based only on unit tests
skip the human microphone test
move to B before A works
```

---

# 15. Definition of Success

The most important final demonstration is extremely simple:

```text
Fresh Samosa installation.

$ samosa wake enroll

Say wake word:
User: "Mirchi"

Again:
User: "Mirchi"

Again:
User: "Mirchi"

✓ Wake word ready.

Samosa returns to idle.

User walks away.

User: "Mirchi"

Samosa:
[immediately begins listening]
```

Then:

```text
restart Samosa
```

and `"Mirchi"` must still work without retraining.

Then repeat enrollment with:

```text
Rekha
```

and it must work without changing models or code.

If that works with acceptable false-positive behavior and low idle resource use, **Method A has achieved the core product goal.**

Everything else is optimization.
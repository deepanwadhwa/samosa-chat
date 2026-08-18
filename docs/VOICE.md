# Local voice for Samosa

## Hands-free mode

Hands-free mode is a real local chain, enabled explicitly from **Settings →
Hands-free voice**:

1. Samosa builds its pinned Whisper.cpp runtime once and verifies its source
   archive.
2. The user downloads the verified 142 MiB English Whisper Base model and, if
   they want a neural reply voice, the optional native Pocket streaming voice.
3. When speech recognition and a reply voice are ready, the **Voice** control
   appears in Chat. One tap begins a conversation; Samosa uses an adaptive
   520–720 ms endpoint based on utterance length and room noise, transcribes
   it, answers aloud, and returns to listening. **End** stops the conversation.
4. The browser resamples the recording to mono 16 kHz PCM WAV and sends it only
   to the token-gated local gateway. The gateway accepts no other media shape,
   runs Whisper.cpp, then deletes the temporary WAV and transcript file.
5. The selected chat model answers normally. In Voice mode, complete phrases
   are queued for speech as model text arrives, so playback can begin before
   the complete answer has finished generating.

The particle overlay is stateful rather than decorative: it swarms into a
samosa while listening, transcribing, thinking, and speaking. It never sends
audio, text, or telemetry to a cloud provider.

Typed conversations are always silent. Each completed reply has a **Listen**
control for deliberate playback, while automatic speech belongs only to an
active Voice conversation.

Pocket TTS is the downloadable neural voice: its int8 model and two reference
voices stay on the Mac and are loaded directly by Samosa's gateway through a
pinned native C API. There is no Python interpreter, pip environment, loopback
voice worker, or cloud speech request for that path. Settings presents a voice
catalog with one active STT choice and one active TTS choice. Users can compare
Whisper Base/Tiny, Pocket, existing Kokoro, MOSS TTS Nano, Kitten TTS Nano, and
browser/macOS speech; every card shows practical pros and cons before
selection. Existing Kokoro installs remain usable as an explicit selection
rather than an invisible fallback.

## Runtime and model details

Use [whisper.cpp](https://github.com/ggml-org/whisper.cpp) for the first local
dictation implementation. It has a C-style API, CPU-only operation, VAD, and
Apple Silicon paths (NEON, Accelerate, Metal, and optional Core ML). It is the
right fit for Samosa's small, local-runtime constraint, without writing a new
ASR engine from scratch.

For an English-first 16 GB Mac, start with `base.en`: whisper.cpp lists it at
142 MiB on disk and roughly 388 MiB memory. `tiny.en` is 75 MiB / roughly
273 MiB and is useful for short commands, but should be an explicit
speed-first option. Do not run ASR concurrently with a large Samosa model by
default; record first, transcribe after the user releases the mic, and pause
or reject when memory pressure is high.

The implementation keeps these boundaries:

- Capture audio only after a user taps the microphone.
- Encode mono 16 kHz PCM WAV in the browser and send it only to localhost.
- Run a pinned `whisper.cpp` binary with a verified model file; return text,
  never keep raw recordings after the request completes.
- Surface the selected model, language, audio duration, and failure state in
  the UI. There is no cloud browser-recognition fallback: that would
  contradict the local promise.

## Custom wake words

Voice settings also includes a local **Custom wake word** flow. Enter a short
word or phrase, choose how many times to say it, then use the guided recorder:
press **Start recording**, say the word while the three-second countdown runs,
and choose **Record again** or **Next recording**. A live meter says when sound
is detected. Every completed sample appears below the guide with a **Play**
button, so recordings can be checked before clicking **Listen for wake word**.
Wake listening is an explicit persistent setting: it stays quietly active in
the background until you click **Stop listening**. Saying the wake word enters
the same multi-turn hands-free conversation as pressing **Voice**: after each
answer, Samosa remains in Voice and listens for the next message. Clicking
**End** leaves Voice and automatically returns to background wake listening.
The armed setting also survives a page refresh or reopening the app in the
same browser; microphone permission is still required. It never starts Voice
by itself.
The compact acoustic templates and optional playback copies stay in this
browser on this Mac; no enrollment audio is uploaded. The detector keeps a
fixed two-second ring, isolates one short speech burst after trailing silence,
and evaluates it with cosine-DTW. Two references must agree, the candidate
duration must resemble the enrolled examples, and the configured match level
is tightened automatically when the enrollment examples are especially
consistent. A short cooldown follows a match.

The detector owns no second microphone. It uses the same Web Audio capture
graph as Voice. Unlike the original prototype, overlapping slices from one
utterance are not counted as independent confirmations. The room-noise gate,
speech boundary, duration check, and multi-recording consensus all run before
Voice opens. The wake word itself is not sent through Whisper, because
arbitrary custom words are often transcribed incorrectly. Once detected, the
graph changes to the existing Whisper → chat → spoken-reply loop and follow-up
speech is sent to the selected local model. Existing `acoustic-template-v1`
profiles with playback copies are migrated locally to the corrected
`acoustic-template-v2` extractor. Native speech-embedding model parity,
threshold calibration, and the required human microphone report remain
release work before calling this a production wake word engine.

## Neural reply voice

Pocket TTS is the default opt-in local English neural voice. Pressing
**Download** fetches a SHA-256-verified int8 Pocket model (98 MB archive), two
pinned CC0 reference voices, and—unless Kokoro already installed it—the pinned
Sherpa-ONNX native runtime. The gateway loads the runtime with `dlopen` and
invokes its C API in-process; it does not start a service or expose a network
listener.

Samosa offers Caro and Stuart. The first coherent sentence is sent to Pocket
immediately while later model deltas continue to arrive. Once playback begins,
Samosa uses the already-queued audio as a latency budget to combine short
sentences and lines into larger, bounded batches. This avoids restarting
Pocket's generative voice state every few words without delaying first speech
or risking a playback gap. The selected reference voice is pinned for the
entire reply. The native endpoint forwards each Pocket progress callback as
little-endian PCM, and the browser schedules every buffer through Web Audio
immediately, without waiting for a complete WAV or complete answer.

On the reference M3, a warm Pocket request produced first PCM in 349–360 ms,
finished generating 5.4 seconds of audio in 734–742 ms, and emitted 5–6
progress callbacks about 0.2 seconds apart. Two CPU threads were faster than
one, four, or six, so two is the default. The equivalent legacy Kokoro request
took about 3.7 seconds to first PCM. Kokoro remains supported for users who
already downloaded it, but Settings describes its chunking and offers an
upgrade rather than presenting it as streaming.

Before speech synthesis, the visual answer is converted to spoken prose. Link
targets are omitted, while colons, semicolons, slashes, and markdown formatting
become pauses or spaces; the voice engine is never handed glyphs that it would
pronounce as “colon” or “slash.”

Repeated-word safety is deliberate. Pocket runs with the normal five-step
flow-matching configuration rather than the unstable one-step shortcut. A
clause containing a run of three identical words—such as “three three
three”—is read by the selected browser/macOS voice instead of the generative
voice. Native PCM is also bounded by a text-relative maximum; if a runtime
returns an unusually long stream, playback stops and the browser reports the
failure instead of scheduling an unbounded ghost loop.

### Browser-local TTS catalog entries

MOSS TTS Nano and Kitten TTS Nano are downloadable and selectable from the same
one-choice TTS catalog. Their JavaScript/WASM adapters are shipped with the
app; their model weights are fetched only after the user clicks **Download**
and are cached in browser-managed origin storage. The C gateway never executes
these models and Samosa does not ship a Python interpreter, pip environment,
or Python adapter.

MOSS uses the upstream browser ONNX reader with deterministic greedy decoding,
and currently exposes its built-in Junhao voice. Kitten uses a browser worker
and exposes its eight built-in voices. Both paths split speech into bounded
phrases and enforce an audio-duration limit before Web Audio playback. Browser
WASM speed and memory use vary by machine, so the catalog cards explain those
tradeoffs beside the pros and cons.

## Timing diagnostics

**Settings → Voice → Voice timing diagnostics** writes private `0600` JSONL
files under `~/.samosa/logs/voice/`. Published release builds remain opt-in:
**Start timing log** begins a debugging session and **Stop and save log** closes
it. A release installed by `tools/install_local_dev.sh` automatically starts a
new trace with each gateway run, so local testing cannot accidentally happen
without diagnostics. Set `SAMOSA_VOICE_TRACE_AUTO=0` to disable that development
default.

Every row uses schema `samosa.voice.trace.v1` and carries a session ID,
sequence, source, event, wall/monotonic timing, and—when applicable—a Voice
turn ID. Fields are restricted to timing, byte/character/sample counts, HTTP
status, model/voice labels, and endpoint measurements. Audio, transcripts,
prompts, and reply text are neither accepted nor written. Browser and gateway
events share the turn ID so analysis can calculate speech endpoint, STT, model
time-to-first-content, TTS time-to-first-PCM, generated-audio duration, and
playback-buffer underruns separately.
Run `tools/analyze_voice_trace.py ~/.samosa/logs/voice/voice-trace-….jsonl`
to print those stages as a per-turn millisecond table.

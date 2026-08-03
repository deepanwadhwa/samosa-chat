# Local voice for Samosa

## Hands-free mode

Hands-free mode is a real local chain, enabled explicitly from **Settings →
Hands-free voice**:

1. Samosa builds its pinned Whisper.cpp runtime once and verifies its source
   archive.
2. The user downloads the verified 142 MiB English Whisper Base model and, if
   they want a neural reply voice, the optional native Kokoro-82M voice.
3. When speech recognition and a reply voice are ready, the **Voice** control
   appears in Chat. One tap begins a conversation; Samosa detects a natural
   pause, transcribes it, answers aloud, and returns to listening. **End**
   stops the conversation.
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

Kokoro-82M is the downloadable neural voice: its int8 model stays on the Mac
and is loaded directly by Samosa's gateway through a pinned native C API.
There is no Python interpreter, pip environment, loopback voice worker, or
cloud speech request. Browser/macOS voices remain a fallback until Kokoro has
been downloaded.

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

## Neural reply voice

Kokoro-82M is an opt-in local English neural voice. Pressing **Download**
fetches a SHA-256-verified int8 Kokoro model (about 103 MB) and a pinned
Sherpa-ONNX native runtime (about 16 MB) into Samosa's private voice directory.
The gateway loads the runtime with `dlopen` and invokes the Sherpa-ONNX C API
in-process; it does not start a service or expose a network listener.

Samosa offers six curated voices: Bella, Nicole, Sarah, Adam, Michael, and
Emma. Voice replies are still phrase-streamed as model output arrives, while
full synthesis remains local and cancellable between phrases.

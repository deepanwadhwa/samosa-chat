# Streaming TTS investigation and decision

## Why Kokoro paused

Samosa already consumed model text deltas and used an HTTP PCM stream, but the
pinned Kokoro engine generally invoked its progress callback only after a
complete phrase. The browser could not play audio that the engine had not yet
produced. An additional 44-character fallback boundary could split a proper
noun such as “United States” across two synthesis requests, making the engine
pause at exactly the wrong place.

Streaming model text, streaming synthesis progress, and streaming browser
playback are three independent boundaries. All three must be incremental to
avoid the old whole-phrase delay.

## Replacement

Samosa now prefers Kyutai Pocket TTS through Sherpa-ONNX's native C ABI. It is
CPU-oriented and produces multiple PCM progress callbacks while generating one
clause. The browser schedules each callback immediately. There is no Python
worker, pip environment, extra localhost service, WAV assembly, or cloud TTS.

The first coherent sentence starts synthesis immediately. After playback has
begun, its queued duration becomes a latency budget: short sentences and lines
are combined into bounded Pocket requests while there is comfortable audio
headroom, and synthesis resumes before that headroom can fall below the
underrun guard. This preserves fast first audio while avoiding the severe
speaker/pitch drift caused by rebuilding Pocket's generative state for every
few words. The selected reference voice is pinned for the whole reply. A
190-character emergency boundary still handles pathological unpunctuated
output without splitting proper nouns at the old 44-character cut.

## Local M3 benchmark

Sentence: “The total land area of the United States is approximately 3.5
million square miles.”

| Engine/configuration | Warm first PCM | Total synthesis | Audio | Progress |
|---|---:|---:|---:|---:|
| Legacy Kokoro, 6 threads, 1.15× | ~3,691 ms | ~5,711 ms | ~7.55 s | phrase-complete |
| Pocket/Caro, 2 threads | 349 ms | 734 ms | 5.36 s | 5 callbacks, max gap 226 ms |
| Pocket/Bria, 2 threads | 360 ms | 742 ms | 5.44 s | 6 callbacks, max gap 215 ms |

Pocket thread sweep (warm run): one thread 952 ms total, two threads 742 ms,
four threads 759 ms, six threads 1,279 ms. Two threads is therefore the
default; `SAMOSA_POCKET_THREADS` remains available for controlled benchmarks.

The pinned int8 model archive SHA-256 is
`2f3b88823cbbb9bf0b2477ec8ae7b3fec417b3a87b6bb5f256dba66f2ad967cb`.
Reference voices are pinned from Kyutai's CC0 voice-zero collection.

Primary references:

- [Kyutai Pocket TTS](https://github.com/kyutai-labs/pocket-tts)
- [Kyutai Pocket TTS documentation](https://kyutai-labs.github.io/pocket-tts/)
- [Sherpa-ONNX Pocket TTS integration](https://k2-fsa.github.io/sherpa/onnx/tts/pocket.html)
- [Kyutai TTS voices](https://huggingface.co/kyutai/tts-voices)

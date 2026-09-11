# One-hour local audio acceptance — 2026-09-06

## Result

Pass. A clean isolated branch gateway transcribed a synthetic one-hour English
WAV with the installed Whisper Base English model, published seven durable
overlapped checkpoints covering exactly 0–3600 seconds, restarted, and answered
through the installed Qwen3.6 35B A3B model. The answering turns reused the
transcript cache and did not modify or add checkpoints.

The real answering model returned:

- early: `cedar71`, cited at `00:00:30.000–00:01:00.000`;
- middle: `Mango 4.2`, cited at `00:29:57.000–00:30:27.000`;
- late: `quartz nine six`, cited at `00:58:55.000–00:59:25.000`;
- absent fact: “The transcript does not contain a papaya verification code.”

No source bytes or evidence left the machine.

## Fixture and coverage

- Source: mono 16 kHz PCM16 WAV, 3,600.000 seconds, 115,200,078 bytes.
- Attachment SHA-256: `8c740f8a3695f6c10b852864801558526a4c95a27372a7078c213cd6a32d10df`.
- Spoken markers were synthesized at 30, 1,800, and 3,540 seconds.
- Window bounds: `0–600`, `599–1199`, `1198–1798`, `1797–2397`,
  `2396–2996`, `2995–3595`, and `3594–3600` seconds.
- Final evidence: `samosa.evidence.v1`, 121 timestamped segments,
  `coverage.complete=true`, `coverage.start_seconds=0`, and
  `coverage.end_seconds=3600`.
- The six long checkpoints in the interrupted discovery run were reused
  byte-for-byte while only the missing 3,594–3,600 second tail was produced.
  A separate clean run then produced all seven checkpoints from scratch.

Whisper sometimes rendered long silence as `you` and repeated the middle
phrase after its actual occurrence. This is model output, not invented gateway
text. The bounded evidence remained source-linked, and term-balanced retrieval
kept the genuine early and late markers from being crowded out by repetition.

## Models and machine

- Whisper model ID: `voice-stt-whisper-base-en`.
- Whisper model SHA-256:
  `a03779c86df3323075f5e796cb2ce5029f00ec8869eee3fdfb897afe36c6d002`.
- whisper.cpp runtime SHA-256:
  `b6aea409400da5f8c2d2dde7e4f31ff1f3ac98a65a4b8fe4ce0408bf3ae629b7`.
- Answerer: Qwen3.6 35B A3B, `groupwise-symmetric-q4-v1` catalog build.
- Qwen runtime SHA-256:
  `8d03951b9bb67d7824b9254cb00740cccb1aa95babf0b49c200545c2f154e7e8`.
- Machine: `Mac15,12`, Apple M3, 16 GiB RAM, arm64.
- OS: macOS 26.5.1 (25F80).

## Measurements

- Upload over loopback: 1.651 seconds.
- Clean seven-window transcription and deterministic handoff: 28.033 seconds.
- First durable window/progress: approximately 4–5 seconds after request
  start (checkpoint timestamps have one-second resolution).
- Peak gateway/backend/Whisper process-tree RSS during transcription:
  447.1 MiB.
- First real Qwen fact answer: 137.239 seconds gateway wall time;
  measured process-tree peak RSS 3,496.2 MiB.
- Real Qwen absent-fact refusal: 102.181 seconds gateway wall time.
- Swap before transcription, after transcription, before Qwen, and after both
  Qwen answers: 0 MiB used; swap delta 0 MiB.
- After shutdown: no gateway, Qwen, fake backend, or Whisper listener/child
  remained.

## Defects found and closed by the gate

1. Window count used `ceil(total/window)` while traversal advanced by
   `window-overlap`; an exact one-hour source therefore stopped at 3,595
   seconds. Count and traversal now use the same stride, with an exact-boundary
   regression.
2. whisper.cpp can timestamp a short final chunk against its 30-second decode
   frame. The validator now accepts only that bounded shape and clamps it to
   actual source duration; arbitrary distant timestamps still fail.
3. Aggregate lexical scoring allowed repeated middle evidence to consume all
   retrieval anchors. Each distinct requested term now receives an anchor
   before the remaining relevance budget is filled.
4. The enlarged attachment trace fields overflowed a 64-byte staging buffer
   and produced invalid JSONL. The buffer is now sized for the full typed
   capability record; the final Developer trace parses completely as JSON.

Deterministic coverage for these cases lives in
`tests/test_audio_attachments.sh`.

# Roadmap

What is next, roughly in order of how much it would change things. The
[README](../README.md#roadmap) summarises this.

## Roadmap

Nothing here is promised or dated. This is what the project wants to become,
roughly in order of how much it would change things.

### Make x86 fast (Linux and Windows now work — they are just slow)

**This shipped.** Linux and Windows run Samosa via Docker: the build and test
suite are green on Debian 12 and Ubuntu 26.04, and a real chat is confirmed on
one Windows laptop (i7-1260P) under WSL2. The memory-pressure watcher, the
thread policy, and the installer all learned Linux and cgroups along the way.

**What is left is speed, and the cause is known.** The AVX2 kernels sit right
next to the NEON ones — but the build never passes `-march`, so on x86 they are
compiled out and the engine runs a scalar loop, measured **7.6x slower**. The
Zenbook above has AVX2 and gets none of it: **1.26 tok/s against 5–7 on the M3.**

The fix is runtime CPU dispatch (`cpuid` at startup, not a compiler flag —
one Docker image has to run on many CPUs). Tracked as **G10 / H2** in
[docs/TASKS_HARDWARE.md](docs/TASKS_HARDWARE.md). Measured on the M3, decode is
70% SSD wait and 30% matmul; on x86 the scalar path inverts that to ~77%
compute, so removing it should take x86 from compute-bound back to
storage-bound — the same regime as the Mac.

### Vision

Qwen3.6 is natively multimodal, and the engine has no vision runtime — so Samosa
is text only today.

**But the vision tower is already on your disk.** The converter *intended* to skip
it: its filter tests for the substring `vision`, and Qwen names those tensors
`model.visual.*`, so the filter never matched. All 27 blocks — 444 tensors,
0.454 GB — were quantized and shipped inside `resident.safetensors`. Every
install already has them, inert.

They also work: a numerical parity check against the upstream BF16 reference
measured **mean cosine 0.9976, min 0.9923** — the accidentally-quantized weights
are usable as they are.

**The goal is to add image input back.** It means building a vision encoder
alongside the language engine — roughly "colibrì, but for vision": an image
encoder, patch embedding, and a projector into the language model's space.
The tokenizer still carries Qwen's image and video tokens, so the language side
is already ready for it.

### Metal (Apple GPU)

The engine is CPU-only today. Metal should help, but it is not free speed: much
of a long answer is spent streaming expert weights from the SSD, and a GPU does
not make those reads faster. The plan is to move the expert matrix multiplies to
Metal while CPU threads keep feeding them from disk, then measure end-to-end
tokens/sec, SSD reads, memory, thermals, and battery — not just an isolated fast
matmul. The CPU path stays as the correctness fallback.

### A more mature app

The web app is a demo. To become a real interface it needs conversations kept in
RAM instead of re-read from disk each turn, transcript management on the server,
and deleting a chat should remove its saved snapshot, not just the browser copy.
Chatting over a document and web access are wanted after that.

### Real quality evidence

Group-32 is measured on one machine against one reasoning control. It needs a
proper benchmark suite, comparison against upstream across task types, and a
bounded test for very long answers (a crash above 4,096 tokens was fixed, but
the regression test for it is not written yet).

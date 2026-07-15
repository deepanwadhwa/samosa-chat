# Performance: speed, memory, and storage

Every number here was measured on the machine named beside it. The
[README](../README.md#where-it-runs-and-how-fast) summarises this.

## Where it runs, and how fast

Every number below was measured, on the machine named next to it. Nothing here
is extrapolated.

| Platform | How | Measured decode | Verified on |
|---|---|---|---|
| **macOS, Apple Silicon** | native installer | **5–7 tok/s** | one 16 GB M3 MacBook Air (fanless), 2-thread default |
| **Linux, x86_64** | Docker | *not yet measured* | build + test suite green on Debian 12 and Ubuntu 26.04 |
| **Windows, x86_64** | Docker (WSL2) | **1.26 tok/s** | one ASUS Zenbook, i7-1260P, 16 GB, Docker CE inside WSL2 |
| Linux/macOS, arm64 | Docker | ~0.9 tok/s | penalised by a host bind mount; use a named volume |

**macOS is the fast path. Linux and Windows work, and today they are ~4–5x
slower — for a reason we understand and can fix.**

The engine's vectorised kernels are selected at compile time, and the build does
not pass `-march`, so on x86 the AVX2 kernels are never compiled in and the
engine falls back to a scalar loop — measured **7.6x slower** than the vectorised
path on identical hardware. The Zenbook above *has* AVX2; the build throws it
away. That is tracked as **G10 / H2** ([docs/TASKS_HARDWARE.md](docs/TASKS_HARDWARE.md)),
and the fix is runtime CPU dispatch rather than a compiler flag, because one
Docker image has to run on many different CPUs.

So the honest summary: **on Apple Silicon this is a usable chat app. On x86 it is
a working one.** A short factual answer is fine; a long reasoning answer at
1.26 tok/s is a coffee break. If that matters to you, wait for H2.

**"Runs on the CPU" does not mean it runs on any 16 GB laptop.** What it needs:

- **CPU:** Apple Silicon, or x86_64 (AVX2 strongly recommended — without it,
  slower still), or arm64.
- **RAM:** 16 GB, with **≥6 GB given to the Docker VM** on Linux/Windows. The
  ~2 GB default cannot load the model at all.
- **Storage: an NVMe SSD.** Expert weights stream from disk on every token, so
  storage bandwidth is the main driver of speed — a host bind mount instead of a
  named Docker volume costs about **6x**, measured. A hard drive is unusable.

The output is identical across all of them: the same prompt and seed returns the
same tokens on macOS/NEON, arm64 Linux, and x86_64 Linux, at the same ~3.84 GB
footprint. The difference is speed, not behaviour.

## Speed

All numbers are from one fanless MacBook Air M3 with 16 GB of RAM. They describe
specific runs on this one machine, not a guarantee for other machines.

The product is group-32, so those numbers come first:

| group-32 task | threads | speed |
|---|---:|---:|
| direct answer | 2 | 7.27 tokens/sec |
| 933-token thinking answer | 2 | 4.85 tokens/sec |
| 5,000-token code page | 4 | 6.47 tokens/sec |

For reference, the older published model runs slightly faster because it is
smaller: about 7–8 tokens/sec on 2 threads, about 9.5 on 4 threads (`--fast`),
and 14–24 tokens/sec for prefill.

Decode is the speed of writing the answer. Prefill is the speed of reading your
input before it starts. Prefill is the slow part for long inputs: reading a
5,000-token document once takes about 3.5–6 minutes. Saved conversations mean a
document is read only once.

## Memory use

On the command-line tool, older runs used about 2.5–3 GB and group-32 runs used
about 3.2–3.9 GB.

In the app, memory grows in three stages:

1. **Model loaded, no chat yet:** about **2.5 GiB**.
2. **After the first answer:** about **3.9 GiB**. The first answer fills the
   expert cache, which adds about 1.3 GB and then holds steady.
3. **As a conversation gets longer:** memory rises slowly with the length of the
   conversation you are in. In one test it rose about 143 MB while a single chat
   grew from 176 to 1,017 tokens. The expert cache stayed flat at 1.29 GB the
   whole time.

That growth is bounded, not a leak. The per-token memory (the KV cache) is about
40 KiB per token across the 10 attention layers. The measured rise is a little
higher because the memory allocator holds on to its high point. For a
conversation of fixed length, memory levels off — an eight-turn test on the same
length held at **3.91–3.92 GiB**. The 24,576-token limit caps the worst case at
roughly **5–5.5 GiB** after a maximum-length chat. Only one conversation is in
memory at a time.

The memory number the app shows is the real macOS "physical footprint," which
matches Activity Monitor.

A note on swap: on macOS, swap can stay in use from an earlier busy period even
after memory frees up. macOS does not shrink the swap file or pull that data
back on its own. So swap being in use does **not** by itself mean Samosa is
swapping now. The signal to trust is green memory pressure.

Each saved turn writes a 63–70 MB sealed file to disk. The model files
themselves are read-only.

## SSD speed: the one thing to be deliberate about

This is the most important part for understanding the performance and resource footprint of your machine, so it is stated plainly.

Samosa keeps its memory footprint small by **not** holding all 35B parameters in RAM. Instead, it reads each token's expert weights from the SSD as the model chooses them. The longer an answer is, the more expert data it reads.

The amount of data read is large. One 933-token thinking answer read **376 GB** of expert data from the SSD.

### Does this wear out the SSD? No — and the comparison people reach for is backwards

Reading 376 GB sounds alarming, so this section used to say that expert streaming
is what wears the drive. **That was wrong, and it is corrected here.**

**SSD lifespan is consumed by writes, not reads.** Flash endurance is rated in
**TBW** (Terabytes *Written*, per the JEDEC JESD218 standard) or **DWPD** (Drive
*Writes* Per Day). Every published endurance figure is a write figure — no
manufacturer rates a drive for reads, because program/erase cycles are what wear
out NAND cells. Reads consume approximately zero drive life.

The honest caveat: *read disturb* is a real physical effect — reading a block
slightly perturbs neighbouring cells, and the controller refreshes the block after
a threshold, which is a write. But those thresholds are on the order of tens of
thousands to millions of reads **of the same block**. 376 GB spread over a 20.9 GB
file is about 18 reads per byte. It is orders of magnitude away from mattering.

Which inverts the swap comparison this section used to make. Over that same
session the whole system — Samosa, editor, browser, everything — wrote under about
9 GB to swap. Those **9 GB of writes consume more drive life than the 376 GB of
reads do.** The scary number was the wrong number.

*How we know:* from the definition of the endurance rating, not from a measurement
on the reference machine — Apple Silicon's internal NVMe does not expose SMART
endurance counters to userspace, so `Data Units Written` / `Percentage Used`
cannot be read there. On a Linux machine with `smartctl -A /dev/nvme0`, a long
generation moves `Data Units Read` by hundreds of GB while leaving
`Data Units Written` and `Percentage Used` essentially unchanged. If you run
Samosa on such a machine, you can check this yourself.

The genuine resource considerations are:
- **SSD Speed:** This is the single biggest driver of Samosa's performance. High-speed native NVMe storage (2.3+ GB/s) streams at **5–7 tokens/second**. Slower storage paths (like a Docker virtiofs host bind mount at ~0.5 GB/s) drop this to **~0.9 tokens/second**. SATA SSDs or HDDs are severely bottlenecked or unusable.
- **Power and Heat:** Streaming hundreds of gigabytes per generation keeps the CPU and storage controller active, which drains battery and generates heat.
- **Page Cache Eviction:** Heavy read operations evict other files from the OS page cache, which can temporarily slow down other active applications.

What this means for you:
- **SSD speed matters enormously.** Run Samosa on a fast internal NVMe drive (or a native Docker volume), never on a slow host shared mount or external SATA SSD.
- **Use direct mode for efficiency.** A short factual answer reads very little. A long reasoning turn reads a lot. Use direct mode (`thinking: off` in the app, `--direct` on the command line) when you do not need step-by-step reasoning to conserve battery and keep the machine cool.
- **The default is 2 threads** so the machine stays cool. `--fast` turns on adaptive thermal thread control to scale up performance safely when thermal headroom allows.

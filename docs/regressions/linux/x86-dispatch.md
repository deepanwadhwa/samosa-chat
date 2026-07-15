# G10: the AVX2 kernels are dead code in every shipped x86 build

Ran on: 2026-07-15
Question this answers: *"Does it need to be a Mac? Can any machine with Docker
run it? Doesn't it depend on the SSD?"*

## The finding

**With the shipped build flags, an x86_64 user gets neither AVX2 nor NEON — they
get an unvectorized scalar loop. The AVX2 and AVX512-VNNI kernels never compile
in.**

`install.sh` compiles the engine on the user's machine with:

```sh
$COMPILER -O3 -pthread $OMP_FLAGS -Wno-unused-function ...
```

**No `-march`.** The `Makefile` is the same (`$(CC) -O3 ...`). Verified on
x86_64:

```
gcc -O3             ->  __AVX2__ NOT defined
gcc -O3 -march=native ->  __AVX2__ defined
```

The dispatch in [kernels.h:66-78](../../../src/kernels.h#L66-L78) has **no
`#else`**:

```c
static inline void matmul_q(...){
  for (int o=0;o<O;o++){ ... float a=0; int i=0;
#ifdef __AVX2__
      ... vectorized, advances i ...
#elif defined(__ARM_NEON)
      ... vectorized, advances i ...
#endif
      for(;i<I;i++) a+=xs[i]*(float)w[i];   /* scalar remainder */
```

On x86 with `-O3` alone, neither branch exists, `i` stays 0, and the scalar
remainder does **100% of the work**. The same holds for `matmul_i4`,
`matmul_i2`, and the int-dot kernels.

## Measured cost: 7.6×

Identical hardware (arm64 Linux container), `matmul_q`, I=2048 O=2048 S=1, 300
reps, single thread. The scalar path was forced with `-U__ARM_NEON`:

| Path | ms/call | GFLOP/s | checksum |
|---|---|---|---|
| NEON | **0.49** | **17.09** | 0.001187 |
| SCALAR (no SIMD compiled in) | **3.70** | **2.26** | 0.001189 |

**7.6× slower.** Checksums agree to five decimals, so the scalar path is
numerically equivalent — the difference is purely throughput.

This is a NEON-vs-scalar measurement used as a **proxy** for AVX2-vs-scalar. AVX2
is 256-bit (8 floats/op) against NEON's 128-bit (4 floats/op), so the real x86
gap is plausibly *larger*, not smaller. **Not measured — no x86 hardware is
available.**

End-to-end impact is less than 7.6× because the workload is partly SSD-bound, but
matmul is a large share of decode. An x86 user on a fast NVMe would plausibly see
low single-digit tok/s where the hardware could deliver much more — **slow because
of a missing compiler flag, not because of their machine.**

## This corrects the "restoration, not a port" framing

[TASKS_LINUX.md](../../TASKS_LINUX.md) argued Linux was low-risk partly because
"`kernels.h` already carries `__AVX2__` and `__AVX512VNNI__` paths for every hot
kernel". **The code is there; the shipped configuration never reaches it.**
Inherited-from-colibrì AVX2 kernels that never compile provide no coverage and no
confidence. The x86 hot path is not "already written" — it is unreachable.

## E-L1 cannot be run on this hardware

An amd64 container on this Apple Silicon Mac reports:

```
avx2:        NO
avx512_vnni: NO
sse4_2:      NO
```

Docker Desktop's x86 emulation exposes none of them, so the AVX2 kernels cannot
execute here **even under emulation**. **E-L1 (x86 numerical parity) requires
real x86 hardware.** There is no way around this on the current machine.

Note the ordering trap: fixing G10 (adding `-march`) is what *activates* the AVX2
kernels — code that has never executed anywhere. **G10 makes E-L1 mandatory
rather than optional.** Today x86 is slow-but-probably-correct; after G10 it is
fast-and-unverified.

## Fix directions

1. **Runtime dispatch (preferred).** Compile the SIMD variants with per-function
   target attributes (`__attribute__((target("avx2")))`) and select on `cpuid` at
   startup. One binary, correct on every x86, uses AVX2 where present. More work;
   no baseline decision needed; matches how the engine already probes hardware.
2. **`-march` at install time.** `install.sh` compiles on the user's machine, so
   `-march=native` is available and is the natural fit. Risk: the installer's
   binary is then non-portable (irrelevant — it never moves), and `native` on an
   exotic CPU can miscompile. `-march=x86-64-v3` is the conservative middle
   (requires AVX2, ~2013+ Intel Haswell / 2015+ AMD Excavator).
3. **Probe-and-fallback.** Try `-march=x86-64-v3`, verify the binary runs, fall
   back to `-O3` if it SIGILLs. Ugly, but the installer already compiles and
   smoke-tests, so the machinery exists.

For the **Docker image** (D-3) the calculus differs: the image is built once for
many CPUs, so `-march=native` is wrong and **runtime dispatch is the only correct
answer** — or the image ships a baseline build and is honestly slow.

## Storage: this is SSD-bound by design, and it is not optional

The second half of the question. The engine streams 20.9 GB of experts from disk;
every token pulls hundreds of MB. It does not *benefit from* an SSD — it is
unusable without a fast NVMe one.

| Storage path | Read bandwidth | Observed decode |
|---|---|---|
| ext4 + O_DIRECT on NVMe ([st.h:94](../../../src/st.h#L94)) | 2.3+ GB/s | the 5–7 tok/s design point |
| ext4-in-VHDX, buffered ([st.h:94](../../../src/st.h#L94)) | ~0.8 GB/s | WSL2/Docker disk |
| virtiofs bind mount (measured 2026-07-15) | **0.55–0.64 GB/s** | **0.96 tok/s** |

The last row is the cleanest evidence: same CPU, same model, same binary — **6×
slower purely from the storage path**. A SATA SSD (~550 MB/s) lands in that same
territory. **A spinning HDD is hopeless**: random 16 KB reads at ~100 IOPS ≈ 1.6
MB/s, so hundreds of MB per token means *minutes* per token.

On Windows this compounds: Docker's VHDX sits on the host disk, so the user's
drive *and* the virtiofs/9p layer both apply — which is why D3 in
[TASKS_WINDOWS.md](../../TASKS_WINDOWS.md) requires a named volume, never a `C:\`
bind mount.

## The honest hardware gate

Not "any machine with Docker":

- **CPU:** x86_64 **with AVX2** (2013+ Intel Haswell / 2015+ AMD), or arm64.
  Below that, the scalar path at ~2.26 GFLOP/s.
- **RAM:** 16 GB+, with **≥6 GB given to the Docker VM** (it defaults to ~2 GB,
  which cannot load the model at all).
- **Storage: NVMe SSD.** SATA SSD is degraded; HDD is unusable; network storage
  is unusable.
- **Model on a named Docker volume**, never a host bind mount.

None of this is currently stated anywhere a user would see it.

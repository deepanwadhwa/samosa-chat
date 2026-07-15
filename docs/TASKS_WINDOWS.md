# Issue #2 — Windows, via Docker

Read [ISSUE_TASKS.md](ISSUE_TASKS.md) first, then
[TASKS_LINUX.md](TASKS_LINUX.md).

## Decision (2026-07-15, project owner)

**Windows ships as a Docker container. No native Windows installer, no MSVC or
MinGW port.** Docker runs on Windows; the container runs the Linux build.

This is a good call and it collapses the issue: the earlier three-way analysis
(WSL2 / MinGW-w64 / MSVC) estimated ~3–6 weeks for a native port — winsock,
`pread` emulation, C11 atomics, pthreads, a PowerShell installer. **All of that
is now out of scope.** What remains is issue #1 plus a Dockerfile.

**#1 is a hard prerequisite.** Docker Desktop on Windows runs a Linux VM (WSL2
backend), so the container is running the Linux build. Every gap in
[TASKS_LINUX.md](TASKS_LINUX.md) is a gap here. Docker does not route around the
Linux work — and as D1 below shows, it makes the hardest part of it *mandatory*.

## Do I see any issues? Yes — two blockers and two that shape the UX

None of them threaten the approach. Two need code changes that #1 does not
otherwise force.

### D1 — Container memory limits vs. the expert cache  **BLOCKER**

This is the one that will bite hardest, and Docker makes it strictly worse than
plain Linux.

[qwen36b.c:1877-1882](../src/qwen36b.c#L1877-L1882) sizes the byte-budget expert
cache from `/proc/meminfo` `MemAvailable`. **Inside a container, `/proc/meminfo`
reports the host's memory, not the container's cgroup limit.** On a 32 GB
Windows host with `--memory=8g`, the cache would budget itself against ~32 GB,
sail past 8 GB, and get `SIGKILL`ed by the cgroup OOM killer — no warning, no
cleanup, no useful error. The user sees the container die.

And there is no safety net: `ecache_service_pressure()`
([qwen36b.c:1927](../src/qwen36b.c#L1927)) has its **entire body inside `#ifdef
__APPLE__`**, so on Linux — and therefore in the container — there is no reclaim
path at all.

Confirmed by grep: **nothing in `src/` reads cgroups today.** Zero matches for
`cgroup`, `memory.max`, `memory.current`, or `_SC_NPROCESSORS`.

**Consequence for planning:** gaps G2 and G4 in [TASKS_LINUX.md](TASKS_LINUX.md)
are not "important Linux gaps" any more — they are **Docker prerequisites**. Read
`/sys/fs/cgroup/memory.max` and `memory.current` (falling back to `/proc/meminfo`
when unlimited or on a bare host), and `/sys/fs/cgroup/cpu.max` for threads.
This is the classic containerized-inference bug and it is worth getting right
once.

### D2 — The listener is hard-bound to loopback  **BLOCKER**

[samosa_http.h:211](../src/samosa_http.h#L211):

```c
address.sin_port=htons((uint16_t)port); address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
```

Hardcoded, not configurable. [SERVE_API.md:80](SERVE_API.md) states it as a
safety property: "The listener is hard-bound to IPv4 loopback."

**Inside a container, loopback is the container's own network namespace.**
`docker run -p 8642:8642` forwards to the container's `eth0`, not its `lo`, so
the published port connects to nothing. **Samosa in a container is unreachable
today.** This needs a code change before any Dockerfile can work.

The fix must not quietly discard the safety property:

- Add `SAMOSA_BIND`, **defaulting to `127.0.0.1`** so native macOS/Linux
  behavior is byte-for-byte unchanged.
- The image sets `SAMOSA_BIND=0.0.0.0` — safe, because the container's network
  namespace is the boundary.
- **Document the published form as `-p 127.0.0.1:8642:8642`, not `-p
  8642:8642`.** The latter binds the host's `0.0.0.0` and exposes a 35B model
  server to the LAN. This is a real footgun and the docs must lead with the safe
  form.
- If `SAMOSA_BIND` is non-loopback, log a single explicit line at startup saying
  what it is bound to. The product principle is "local-only by default; the
  internet features reach out, nothing reaches in" — a user who widens that
  should see it happen.
- `--network host` plus `0.0.0.0` collapses the namespace boundary. Warn.

### D3 — Where the 24 GB model lives decides whether this is usable

Three options, and only one is right:

| Placement | Result |
|---|---|
| Baked into the image | 24 GB image. Registry pulls, layer storage, rebuild on any change. **No.** |
| Bind mount from `C:\...` | Goes through the 9p/virtiofs bridge — **this is exactly the slow path [st.h:94](../src/st.h#L94) measured at ~0.8 GB/s vs 2.3+**. Also overlayfs/9p may not support `O_DIRECT`, so the twin fd fails and falls back to buffered ([st.h:87](../src/st.h#L87) — graceful, but you lose the 2.3 GB/s). **No.** |
| **Named Docker volume** | Lives in the WSL2 ext4 VHDX. Fast path, `O_DIRECT` works. **Yes.** |

The engine is SSD-bound — expert streaming is the workload — so this is not a
tuning detail. A user who bind-mounts the model from `C:\Users\...` gets a
fraction of the throughput and blames the model. **The docs must lead with the
named volume, and `samosa doctor` should detect and warn when the model is on a
9p mount.**

### D4 — Docker Desktop's memory ceiling

The WSL2 backend defaults to roughly 50% of host RAM. On a 16 GB Windows host
that is ~8 GB for the container, against a ~4 GiB resident footprint plus page
cache. The page cache is doing real work here — it is what makes expert
streaming tolerable — so squeezing it means more SSD reads and slower decode.

Needs a documented `.wslconfig` and a `--memory` recommendation, and it
interacts with D1: whatever limit is set, the engine must actually see it.

### Minor, but worth knowing

- **No browser in a container.** `samosa app` calls `open`/`xdg-open`
  ([dist/samosa:41](../dist/samosa#L41)). The container entrypoint must be
  `samosa serve`; the user opens the browser on the host.
- **Multi-arch.** Build `linux/amd64` and `linux/arm64`. An amd64 image on ARM
  Windows or Apple Silicon runs under QEMU emulation — catastrophically slow for
  this workload. Better to fail fast than to emulate.
- **Windows Defender** still scans the VHDX. See E-W2.
- **Docker Desktop licensing** is paid above 250 employees / $10M revenue.
  Podman and Rancher Desktop are drop-in alternatives. One line in the docs.
- **The trust model shifts.** Today the installer compiles C the user can read.
  A published image is a prebuilt binary. Keep the property by having the
  Dockerfile **compile from source at build time** and documenting `docker
  build` as a supported path — then a published image is a convenience, not a
  requirement.
- **Docker simplifies the release machinery.** The atomic-activation design
  (staging → verify → `mv -fh` symlink swap,
  [install.sh:171-174](../dist/install.sh#L171-L174)) exists to make upgrades
  safe. **Image tags do that natively**, and the `mv -fh` BSD-ism (G5 in
  [TASKS_LINUX.md](TASKS_LINUX.md)) never runs in the container path. The model
  volume still needs manifest + SHA-256 verification.

## Tasks

### D-1 — cgroup-aware memory and CPU  ~2 days  **Prerequisite; shared with #1**

Implement G2/G4 from [TASKS_LINUX.md](TASKS_LINUX.md) with cgroup v2 as a
first-class source, not an afterthought: `memory.max` / `memory.current` /
`memory.pressure` (PSI) for the cache budget and reclaim; `cpu.max` for the
thread default. Fall back to `/proc/meminfo` and `_SC_NPROCESSORS_ONLN` on a
bare host.

**Acceptance:** `docker run --memory=6g` with the real model — the expert cache
budgets against 6 GB, not the host; the container does **not** get OOM-killed
across an 8-turn conversation; induced pressure demonstrably reclaims.
`--cpus=2` yields 2 threads.

### D-2 — Configurable bind  ~0.5 day  **Prerequisite**

`SAMOSA_BIND`, default `127.0.0.1`. Startup log line when non-loopback.

**Acceptance:** native behavior byte-identical (default still loopback — verify,
do not assume); `-p 127.0.0.1:8642:8642` reaches the app from a Windows host
browser; the existing socket component test still passes.

### D-3 — Dockerfile and image  ~2 days  **THE GAP — nothing exists**

**Status 2026-07-15: the architecture is proven; the packaging is absent.** A
hand-assembled container ran `samosa serve` with the real model and answered
"What is the capital of France?" → "Paris" **from a browser on the host**, with
`rss_gb` 2.52 fresh / 3.84 after the turn — matching macOS. D-2's `SAMOSA_HOST`
bind works; G6's `xdg-open` works. **The end goal is reachable.**

But **no Dockerfile exists on any branch** (verified by `find` and `git log
--diff-filter=A`), the model was bind-mounted from a developer's Mac, there is no
`pull` path, no image, and no docs. **A user cannot do any of this today.** Full
evidence and the required user flow:
[regressions/linux/docker-product-path.md](regressions/linux/docker-product-path.md).

Two things that run confirmed:

- **Model placement is not a detail.** Bind-mounted through virtiofs the model
  decoded at **0.96 tok/s** vs ~5–7 native. Named volume, not bind mount (D3).
- **The UI says "Your model. Your Mac."** ([assets/app.html:361](../assets/app.html#L361))
  to a Linux/Windows user — a false claim in the product. It is coupled:
  [install.sh:157](../dist/install.sh#L157) smoke-tests by grepping that exact
  string. Fix both together.


Multi-stage: build the engine from source with gcc `-fopenmp`; runtime carries
engine + wrapper + `app.html` + tokenizer. **Model is not in the image.**
Entrypoint `samosa serve`. Multi-arch amd64 + arm64.

Add a `samosa pull` path that populates the model volume using the existing
manifest verification (SHA-256 + size per file, resumable) — that logic already
exists in [install.sh](../dist/install.sh) and should be reused, not rewritten.

**Acceptance:** on a clean Windows 11 + Docker Desktop machine:
`docker volume create` → `pull` → `serve` → real chat in a host browser.
Interrupted pull resumes. Corrupted file detected. Image < 300 MB excluding the
model.

### D-4 — `doctor` for containers  ~0.5 day

Extend [dist/samosa](../dist/samosa)'s `doctor`: detect container
(`/proc/self/cgroup`, `/.dockerenv`), report the cgroup memory limit vs. the
cache budget, and **warn when the model is on a 9p/virtiofs mount** (D3) rather
than a volume. `sysctl -n hw.memsize` ([:139](../dist/samosa#L139)) has no
meaning here.

**Acceptance:** `doctor` correctly reports limits inside a container, and the
9p warning fires on a bind-mounted model.

### D-5 — Documentation  ~0.5 day

**The honest sentence.** This is Linux-in-a-VM on Windows, and the docs must say
so plainly: "Windows: runs via Docker Desktop (which uses a Linux VM)". Never
"native Windows support". Requirements, `.wslconfig` guidance, the named-volume
requirement, and the measured tok/s from E-W1 — and lead with `-p
127.0.0.1:8642:8642`.

## Experiments

### E-W1 — Docker-on-Windows performance reality  ~1 day  **After #1**

Measure on 16 GB and 32 GB Windows hosts: decode tok/s vs native Linux vs the
macOS reference; the 8-turn RSS plateau (per E-L2) *inside* the container;
**named volume vs `C:\` bind mount** — quantify the 9p penalty that
[st.h:94](../src/st.h#L94) implies; and behavior at `--memory=6g` / `8g` / `12g`.

**Deliverable:** the measured sentence for the README, plus the recommended
`--memory` floor. If Docker lands within ~20% of native Linux, this issue is
done and the native question never needs revisiting.

### E-W2 — Windows Defender vs a 20.9 GB streaming read  ~0.5 day  **Underrated**

Real-time protection scanning the WSL2 VHDX, against a file that is streamed
continuously — every token touches `experts.bin`. A plausible catastrophic
slowdown that has nothing to do with the port's quality and will be blamed on it.

Measure decode tok/s with real-time protection on vs. an exclusion for the
Docker/WSL2 VHDX path. If the delta is large, the docs must offer the exclusion
and explain it honestly — an AV exclusion is a real security ask, not a
checkbox.

### E-W3 — cgroup OOM behavior under load  ~0.5 day  **Gates D-1**

Before D-1: confirm the failure. Run the real model with `--memory=6g` and watch
it get OOM-killed — reproduce the bug, then fix it. A fix for a bug nobody
reproduced is a guess.

After D-1: 8 turns at `--memory=6g` with zero OOM kills and a documented cache
budget that tracks the limit.

## Non-goals

- Native Windows binaries; MSVC; MinGW-w64; a PowerShell installer. **Decided
  out.**
- Windows containers (as opposed to Linux containers on Windows). The engine is
  POSIX; this would resurrect the entire native port.
- Kubernetes/Compose orchestration. One container, one user, one laptop.
- Docker as the *primary* path on macOS or Linux. Both have native installers;
  Docker on macOS adds a VM and virtiofs overhead for nothing.

## Open questions

- **Does the 16 GB floor survive containerization?** The host needs RAM for
  Windows *and* the Docker VM. A 16 GB Windows host giving the container ~8 GB
  is a genuinely different machine from the 16 GB Mac the project is measured
  on. E-W1 must answer this, and the answer belongs in the docs — the current
  claim is [install.sh:18-19](../dist/install.sh#L18-L19)'s 16 GB gate, which
  the container path never executes.
- **Published image, or build-it-yourself?** Publishing to a registry is far
  better UX and shifts the trust model to a binary. Compiling from source in the
  Dockerfile preserves the property for anyone who wants it. Recommend
  publishing *and* documenting `docker build`.
- **Does Docker become the Linux path too?** It would collapse the E-L4
  filesystem matrix into one supported configuration. Tempting, but the native
  Linux installer is nearly free once #1 lands, and Docker on Linux is a real
  dependency to impose. Recommend native primary, Docker available.

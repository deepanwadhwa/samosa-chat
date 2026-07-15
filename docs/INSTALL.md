# Installing Samosa Chat

Full install detail for every platform. The
[README](../README.md#install) has the short version.

## Install

**Find your machine in this table and follow that row. The two paths are
different — do not mix them.**

| Your machine | Install path | Speed |
|---|---|---|
| **macOS, Apple Silicon** (M1 or newer) | [one command](#macos-apple-silicon) | 5–7 tok/s |
| **Windows** | [Docker inside WSL2](#windows) | ~1.3 tok/s |
| **Linux, x86_64 or arm64** | [Docker](#linux) | ~1–2 tok/s |
| Intel Mac, or under 16 GB RAM | not supported | — |

Every path downloads the same 24 GB model and needs **~30 GB free disk**.
See [Where it runs, and how fast](#where-it-runs-and-how-fast) for why x86 is
slower and what is being done about it.

### macOS (Apple Silicon)

```sh
curl -fsSL https://huggingface.co/deepanwa/Samosa-Chat-Qwen3.6-35B-A3B-group32/resolve/main/install.sh | sh
```

Then **open a new terminal** and ask it something:

```sh
samosa "explain how DNS works"
```

You need an Apple Silicon Mac, 16 GB of RAM, Apple's Command Line Tools (for the
C compiler), and about 30 GB of free disk. The installer resumes interrupted
downloads, checks the SHA-256 of every file, compiles the C engine on your
machine, and smoke-tests it before switching the new release live. A corrupt or
interrupted upgrade leaves your existing install untouched. It does not need
administrator rights.

### Windows

Samosa runs as a Linux container. You do **not** need Docker Desktop — Docker
inside WSL2 is simpler and avoids Docker Desktop's startup problems entirely.

**1. Install Ubuntu on WSL2.** In **PowerShell**:

```
wsl --install -d Ubuntu
```

Reboot, then launch **Ubuntu** from the Start menu. Your prompt changes from
`PS C:\...>` to `you@machine:~$` — **everything below runs there, not in
PowerShell.**

> If `wsl --install` fails, virtualisation is disabled in your BIOS/UEFI. Reboot
> into it and enable Intel VT-x / AMD-V ("SVM Mode"). Nothing else will work
> until you do, and the error messages will not tell you this.

**2. Install Docker inside Ubuntu:**

```sh
curl -fsSL https://get.docker.com | sh
sudo usermod -aG docker $USER
newgrp docker
docker run --rm hello-world
```

`sudo usermod -aG docker $USER` is the step everyone misses — without it you get
`permission denied` on the Docker socket. `newgrp docker` applies it to the
current shell.

**3. Check you have enough memory** — still in Ubuntu:

```sh
free -g
```

Total must be **≥6**. WSL2 takes ~50% of your RAM by default, so a 16 GB laptop
gives ~7–8 and needs nothing further. If it says 2–3, create
`%USERPROFILE%\.wslconfig` in **Windows** (Notepad):

```ini
[wsl2]
memory=8GB
```

then run `wsl --shutdown` in **PowerShell** and reopen Ubuntu.

**4. Install and run Samosa** — in Ubuntu:

```sh
git clone https://github.com/deepanwadhwa/samosa-chat
cd samosa-chat
docker build -t samosa .
docker volume create samosa-model
docker run --rm -v samosa-model:/model samosa pull
docker run -d --name samosa -p 127.0.0.1:8642:8642 -v samosa-model:/model --memory=6g samosa serve
```

The `pull` is 24 GB (~20 min) and **resumes** — if it drops, re-run the same
line. Set `--memory` about 1 GB below what `free -g` reported.

**5. Open http://127.0.0.1:8642** in your normal Windows browser. WSL2 forwards
localhost, so it just works.

`sudo service docker start` after a reboot if Docker is not running.

### Linux

Same as Windows from step 2 onward — install Docker, then the six commands. The
native `curl | sh` installer supports Linux in this repository, but the copy
published on Hugging Face is still macOS-only, so **use the Docker path** until
that is republished.

### Both Docker paths: three things that matter

**Use a named volume, never a folder.** `-v samosa-model:/model` — not
`-v /home/you/models:/model` or a Windows path. The file-sharing layer costs
about **6x**, measured. `docker exec samosa samosa doctor` warns you if you get
this wrong.

**Publish as `-p 127.0.0.1:8642:8642`, never `-p 8642:8642`.** The second form
binds `0.0.0.0` and exposes the model server to your whole network.

**Try more threads — it is probably free speed.** The default targets *half your
performance cores*, which is a comfort setting tuned for a fanless MacBook Air.
It is almost certainly too conservative for a desktop or a mains-powered laptop:
on a 12-core i7-1260P it picks **2 threads**.

```sh
docker rm -f samosa
docker run -d --name samosa -p 127.0.0.1:8642:8642 -v samosa-model:/model --memory=6g -e OMP_NUM_THREADS=8 samosa serve
```

Why this is worth more on x86 than it is on a Mac: the Mac is storage-bound
(70% of decode is SSD wait), so more threads bought only **14%** there. x86 runs
the scalar path today, which makes it *compute*-bound — so threads should pay
much better. Try 4, then 8. Past your performance-core count you are into
efficiency cores and it will flatten. It will run warmer; that is the trade.

Time a run before and after with the same prompt and seed:

```sh
curl -s http://127.0.0.1:8642/v1/chat/completions -H 'Content-Type: application/json' \
  -d '{"messages":[{"role":"user","content":"What is the capital of France?"}],"thinking":"off","max_tokens":16,"seed":11}'
```

The `tokens_per_second` in the reply is your number. If you find a good setting
for your CPU, [open an issue](https://github.com/deepanwadhwa/samosa-chat/issues)
— real numbers from real machines are exactly what the thread policy needs.

### Where Samosa is installed

Everything lives under `~/.samosa`, and nothing is installed system-wide:

| path | what it is |
|---|---|
| `~/.samosa/bin/samosa` | the `samosa` command itself |
| `~/.samosa/current` | symlink to the active release |
| `~/.samosa/releases/` | verified releases, kept so an upgrade can roll back |
| `~/.samosa/chats/` | your saved conversations |

The installer adds `~/.samosa/bin` to your `PATH` by appending one line to your
shell's rc file (`~/.zshrc` for zsh, `~/.bashrc` for bash, otherwise
`~/.profile`). **That only affects terminals you open afterwards** — which is
why the step above says to open a new one. If `samosa` still is not found:

```sh
# make it work in the terminal you already have
export PATH="$HOME/.samosa/bin:$PATH"

# or skip PATH entirely and run it directly
~/.samosa/bin/samosa "how are you"
```

`samosa doctor` reports which release is active and whether the model, engine,
and tokenizer are healthy.

To uninstall, delete `~/.samosa` and remove that one line from your rc file.

The model lives at
[deepanwa/Samosa-Chat-Qwen3.6-35B-A3B-group32](https://huggingface.co/deepanwa/Samosa-Chat-Qwen3.6-35B-A3B-group32).

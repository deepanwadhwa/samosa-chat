# Install Samosa

Samosa's source install is model-less: it installs the app, gateway, and Qwen
engine first, then lets you choose any of the three models from the app or
terminal.

## Requirements

- macOS arm64/x64 or Linux arm64/x64
- a C compiler and OpenMP
- Python 3 (standard library only)
- `curl`
- at least 16 GB RAM is recommended for the three-model workflow
- enough disk for the selected model plus Samosa's 2 GB safety reserve

Model weights require 3.8 GB (Bonsai), 5.6 GB (Ornith), or 24.0 GB (Qwen).
All three require roughly 33.4 GB. Qwen expert streaming benefits strongly from
an NVMe SSD.

## macOS

```sh
brew install libomp
git clone https://github.com/deepanwadhwa/samosa-chat
cd samosa-chat
make install
~/.samosa/bin/samosa app
```

The last command opens <http://127.0.0.1:8642>. Open Settings and use the
Download button beside a model.

`make install` builds the multithreaded Qwen engine and publishes an immutable
development release under `~/.samosa/releases`. If a complete local Qwen
snapshot was already configured through `SAMOSA_SNAPSHOT`, it is hard-linked;
otherwise the installation remains model-less.

## Debian or Ubuntu

```sh
sudo apt-get update
sudo apt-get install build-essential libomp-dev curl python3
git clone https://github.com/deepanwadhwa/samosa-chat
cd samosa-chat
make install
~/.samosa/bin/samosa app
```

CI validates the Linux build and self-contained tests. Real three-model
performance has so far been measured on a 16 GB Apple M3, so do not interpret
CI as a Linux performance claim.

## Terminal model installation

```sh
~/.samosa/bin/samosa models
~/.samosa/bin/samosa pull bonsai
~/.samosa/bin/samosa pull ornith
~/.samosa/bin/samosa pull qwen
~/.samosa/bin/samosa pull all
```

Downloads resume after interruption. Every catalog file is pinned by repository
revision, expected size, and SHA-256. It remains under a `.partial` filename
until validation succeeds. Bonsai and Ornith also install a pinned Prism
llama.cpp runtime for the current platform.

## Files and directories

```text
~/.samosa/
├── bin/samosa
├── current -> releases/dev-…
├── releases/…
├── models/
│   ├── qwen36-group32/
│   ├── bonsai-27b-1bit/
│   └── ornith-9b/
├── backends/prism-b9596-9fcaed7/
├── downloads/
├── chats/
├── config.json
└── gateway-settings.json
```

Interrupted downloads stay in the final model directory with a `.partial`
suffix, or in `~/.samosa/downloads` for the Prism archive. Re-running the same
pull resumes them.

## Add the launcher to PATH

The development installer does not modify shell startup files. Either invoke
the full path or add:

```sh
export PATH="$HOME/.samosa/bin:$PATH"
```

Put that line in `~/.zshrc` or `~/.bashrc` if you want it in future shells.

## Check or update an installation

```sh
samosa doctor
samosa models
```

To update source code:

```sh
git pull
make install
```

The installer stages a new immutable release and atomically changes
`~/.samosa/current`. Downloaded model directories are outside that release and
survive application upgrades.

## Stop the app

```sh
samosa serve --stop
```

## Published Qwen release and Docker

The existing Hugging Face release installer and Docker image are still
Qwen-oriented. They predate app-managed three-model downloads. Use the
source-clone path above for the workflow documented in the current README.

The legacy container flow remains:

```sh
docker build -t samosa .
docker volume create samosa-model
docker run --rm -v samosa-model:/model samosa pull qwen
docker run -d --name samosa -p 127.0.0.1:8642:8642 \
  -v samosa-model:/model --memory=6g samosa serve
```

That path is retained for existing Qwen users; it is not the recommended
three-model installer.

## Troubleshooting

**The app says no model is installed.** This is expected after a fresh clone.
Open Settings or run `samosa pull MODEL`.

**A download stopped.** Run the same pull again. Samosa resumes the `.partial`
file and validates the entire completed artifact.

**The checksum fails.** The file is not installed. Retry. If it repeatedly
fails, check proxies/caches and the repository issue tracker rather than
renaming the partial file yourself.

**Not enough disk.** Free the amount shown by the error. Samosa includes a 2 GB
post-download reserve and will not begin when that reserve cannot be preserved.

**Bonsai or Ornith says the runtime is missing.** Re-run
`samosa pull bonsai` or `samosa pull ornith`; either command provisions the
same pinned Prism runtime.

**Auto context differs from another machine.** That is intentional. Qwen
calculates a safe capacity from RAM and K/V cost. The GGUF runtime fits current
device memory and the gateway reports the selected `n_ctx`.

**The server is already running.**

```sh
samosa serve --stop
samosa app
```

## Uninstall

Stop the server, then remove `~/.samosa` when you intentionally want to delete
the application, all downloaded weights, settings, and durable chats. Model
directories are large, so inspect them before removal if you intend to preserve
weights.

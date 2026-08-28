# Install Samosa

Samosa's source install is model-less: it installs the app, gateway, and Qwen
engine first, then lets you choose any compatible model from the app or
terminal.

## Requirements

- macOS arm64/x64 or Linux arm64/x64 for the core app; VisionPsy and Molmo2's
  first native releases are macOS arm64 (Apple Silicon) only
- a C compiler and OpenMP
- Python 3 (standard library only) for source packaging/development tests; it
  is not copied into or invoked by the installed application
- `curl`
- at least 16 GB RAM is recommended for the multi-model workflow
- enough disk for the selected model plus Samosa's 2 GB safety reserve

Model weights require 3.8 GB (Bonsai), 5.6 GB (Ornith), 5.4 GB (Maple), 24.0 GB
(Qwen), 1.01 GB for auxiliary VisionPsy-Nano, and at most 4 GiB for the optional
native Molmo2 package (macOS arm64). Qwen expert streaming benefits strongly
from an NVMe SSD. VisionPsy runs in a native MLX C++ helper on Apple Silicon.
Molmo2 uses a separate native MLX/AVFoundation helper that exists only while
admitted visual work is running. No Python runtime, `mlx-vlm`, PyTorch, or
Transformers package is shipped or invoked.

## macOS

```sh
brew install libomp
git clone https://github.com/deepanwadhwa/samosa-chat
cd samosa-chat
make install
~/.samosa/bin/samosa app
```

The last command opens <http://127.0.0.1:8642>. Open **Settings → Models** and
use the Download button beside a model. VisionPsy appears under **Vision** only
on a compatible Apple Silicon build; installing it never changes the active
chat LLM. Molmo2 appears in the same category but uses an explicit native-pack
setup instead of an automatic download.

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

VisionPsy is installed from the app's **Vision** category or the inline
**Download and continue** card that appears when an attachment turn needs it.
The latter preserves the exact pending message and attachments, waits for the
verified installation, and resumes that turn once. After installation, visual
inference is offline. Use the same model card's Retry action to repair an
interrupted or invalid installation.

## Optional Molmo2 4B package

Samosa deliberately does not automatically download or convert AllenAI's
approximately 19.4 GB FP32 repository. To enable extended image and video
intelligence, obtain the exact pinned revision on reviewed local storage, then
explicitly run the bundled native converter. The guarded path has been
qualified successfully on the 16 GiB Apple M3 reference Mac:

```sh
~/.samosa/current/bin/molmo2-pack /absolute/path/to/allenai-Molmo2-4B-042abfa \
  ~/.samosa/current/share/molmo2/processor.json \
  ~/.samosa/models/molmo2-4b-mlx-q4-v1
```

The source directory must contain the exact four pinned safetensor shards plus
the pinned `config.json` and `tokenizer.json`. The converter rejects wrong
names, sizes, SHA-256 digests, dtypes, shapes, revisions, extra safetensors, or
processor contracts before publishing. It writes shards no larger than 192
MiB to a staging directory, limits its MLX allocator to 2.3 GiB, and atomically
renames only a complete package. It requires at least 6 GiB free on the output
volume before starting and never overwrites an existing output.

The output keeps the full vision tower and connector in BF16 and quantizes only
large language matrices to affine Q4/group-64. The qualified package contains
3,360,724,138 weight bytes, is 3,372,298,903 bytes in total, and declares
3,763,377,322 bytes of admitted resident memory, with a hard
3,750 MiB runtime ceiling, a 2,048-token per-inference safety envelope, and a
4 GiB package ceiling. Refresh **Settings →
Models** after the package is present. The real-release gate is:

```sh
MOLMO2_MODEL_DIR="$HOME/.samosa/models/molmo2-4b-mlx-q4-v1" \
MOLMO2_SAMPLE_IMAGE=/absolute/path/to/reviewed-image.png \
MOLMO2_SAMPLE_VIDEO=/absolute/path/to/reviewed-video.mp4 \
make test-molmo2-real
```

That gate aborts on critical pressure, any new swap use, or a process-tree
RSS above 3,750 MiB. Do not treat deterministic fixture tests as a
substitute for real-package qualification. The 2026-08-26 reference run passed
conversion, image, video, and live Ornith handoff with zero swap growth; see
[`regressions/molmo2-4b-q4-2026-08-26.md`](regressions/molmo2-4b-q4-2026-08-26.md).

## Files and directories

```text
~/.samosa/
├── bin/samosa
├── current -> releases/dev-…
├── releases/…
├── models/
│   ├── qwen36-group32/
│   ├── bonsai-27b-1bit/
│   ├── ornith-9b/
│   ├── visionpsy/
│   └── molmo2-4b-mlx-q4-v1/
├── current/share/molmo2/processor.json
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

**Molmo2 says native pack required.** This is intentional; there is no trusted
public Q4 artifact in the catalog. Follow the native-pack procedure above. Do
not point the runtime directly at the upstream FP32 shards.

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

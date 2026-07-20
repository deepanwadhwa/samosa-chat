# Models, downloads, and Internet boundaries

Samosa has three inference backends behind one loopback gateway.

## Qwen

Qwen3.6 35B A3B uses Samosa's C expert-streaming engine. Shared weights stay
resident while routed expert weights stream from the SSD. It has the most
Samosa-specific integration, sealed session snapshots, and image input.

Catalog source:
[deepanwa/Samosa-Chat-Qwen3.6-35B-A3B-group32](https://huggingface.co/deepanwa/Samosa-Chat-Qwen3.6-35B-A3B-group32).

## Bonsai

Bonsai 27B is a 1-bit GGUF released by PrismML. It is unusually small for its
parameter count and uses the pinned Prism llama.cpp runtime.

Catalog source:
[prism-ml/Bonsai-27B-gguf](https://huggingface.co/prism-ml/Bonsai-27B-gguf).

## Ornith

Ornith 1.0 9B is a Q4_K_M GGUF from DeepReinforce, aimed at compact coding and
reasoning use. It shares the Prism runtime with Bonsai.

Catalog source:
[deepreinforce-ai/Ornith-1.0-9B-GGUF](https://huggingface.co/deepreinforce-ai/Ornith-1.0-9B-GGUF).

## What model installation accesses

When the user explicitly clicks Download or runs `samosa pull`, Samosa contacts:

- the pinned Hugging Face model revision;
- the pinned Prism GitHub release for a missing GGUF runtime.

Artifacts are defined in `tools/samosa_models.py` with immutable revisions,
byte counts, and SHA-256 digests. The downloader uses resumable partial files
and performs validation before the target path is published.

Model inference itself requires no Internet connection.

## What the app sends

The app sends chat requests to `127.0.0.1`. Chat content is not forwarded to
Hugging Face, GitHub, Qwen, PrismML, DeepReinforce, or Samosa maintainers.

The gateway can optionally fetch a public URL or search provider when the user
uses the Internet-source controls. That is a separate, user-initiated feature:

- local/private network destinations are blocked;
- redirects are revalidated;
- response size and extracted text are capped;
- retrieved text is treated as untrusted source material;
- offline mode disables these requests.

Search provider configuration lives in `~/.samosa/config.json`; API credentials
are never bundled.

## Context and model switching

Only one backend runs at once. Model switching stops the old process first.
Each GGUF conversation ledger is keyed by both chat ID and backend, preventing
Bonsai and Ornith from silently sharing incompatible model-facing state.

Auto context is backend-aware:

- Qwen uses Samosa's memory/KV calculation.
- Prism fits Bonsai and Ornith separately because model weights and K/V costs
  differ. The resulting capacities therefore need not match.

## Compaction privacy

The active local model creates continuation memory. No cloud summarizer is
used. Samosa atomically stores the smaller replacement under
`~/.samosa/chats`; the browser transcript remains visible and local.

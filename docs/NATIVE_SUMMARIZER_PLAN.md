# Native summarizer

Status: implemented and enabled by default in packaged Apple-Silicon releases.

## Runtime

Samosa uses `Falconsai/text_summarization`, a 60.5M-parameter T5-small model,
converted from the official safetensors at revision
`6e505f907968c4a9360773ff57885cdc6dca4bfd` to a 65,364,992-byte Q8_0 GGUF:

```text
sha256 30c73e0b2ebd4f65538d87f5157aa7193261e8ff362e441df86c6941f3e9265c
```

The app does not ship or invoke Python. `samosa-summarizer` is a small native
sidecar linked to pinned Prism/llama.cpp revision
`9fcaed763ccda38ea81068ad9d7f991aaddca451`. It keeps the model resident and
accepts repeated length-prefixed UTF-8 requests over private stdin/stdout
pipes. The gateway starts it lazily, serializes calls, applies a bounded
timeout, restarts it after failure, and stops it with the gateway.

The dedicated helper is necessary because the generic llama-server and
completion front ends at this pinned revision stop T5 generation at the
decoder-start token. Direct `llama_encode`/`llama_decode` execution produces
the correct summary. A release-builder may use Python to convert official
weights; that tooling is not included in or required by an installed app.

Primary model reference: [Falconsai/text_summarization](https://huggingface.co/Falconsai/text_summarization).

## Web research

The local chat model ranks the provider's eight candidates from the resolved
conversational question. The gateway then:

1. fetches up to eight readable article bodies;
2. sentence-aligns bounded 1,400-character chunks;
3. summarizes up to three chunks per article with native T5, then reduces the
   chunk summaries locally into one bounded article digest;
4. carries the fetched title, URL, and a verbatim opening anchor beside every
   generated summary;
5. asks the chat model once whether the combined digests answer every material
   part of the question; and
6. if not, appends up to two model-selected raw page excerpts before final
   synthesis.

Search-result snippets are discovery input only. For medical, legal, and
financial claims, the final grounding contract requires exact facts to come
from verbatim fetched anchors or raw text, never a snippet or generated summary
alone. If the native helper is missing, crashes, times out, or emits an empty
result, research falls back to bounded raw pages rather than blocking the turn.

## Documents and Chutni

Long PDF chat attachments now include a native multi-chunk summary plus a
bounded verbatim opening excerpt. Short documents stay verbatim. Chutni's
`summary_short` enrichment for PDFs, OCR text, text files, and JSON now uses the
same persistent native helper. The active chat LLM is retained only as a
fallback when the native runtime is unavailable; image captions still require
an image-capable chat model.

Native Chutni artifacts record the Falconsai model revision and the recipe
`samosa-native-summary-leading-content-v1`, so stored provenance does not
pretend the active chat model created them.

## Packaging and verification

`tools/stage_native_summarizer_runtime.sh` verifies the exact Prism source
revision and stages the executable plus its shared libraries.
`tools/package_hf.py` rejects the summarizer GGUF unless both its byte size and
SHA-256 match the pin above, then includes runtime and model even in a
runtime-only Samosa release. `dist/install.sh` installs the complete set
atomically under the versioned release and rejects partial runtime manifests.

Useful gates:

```sh
make samosa-gateway test_fake_native_summarizer
make test-native-summarizer-real \
  SAMOSA_SUMMARIZER_TEST_MODEL=/path/to/samosa-text-summarization-Q8_0.gguf
SAMOSA_FAKE_SUMMARIZER="$PWD/build/test_fake_native_summarizer" \
  sh tests/test_web_search.sh
```

The real-model gate pins a factual regression: a source saying 30 South
Carolina cases must retain `30 cases` and must not invent `29`.

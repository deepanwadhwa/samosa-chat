# Native article summarizer plan

Status: runtime path identified; integration blocked on a trustworthy GGUF and
quality gates.

## Decision

Use the Prism `llama.cpp` runtime Samosa already provisions for Bonsai and
Ornith. The installed Prism revision supports the `t5` encoder/decoder
architecture, including `T5ForConditionalGeneration`, so the application does
not need Python, Transformers, ONNX Runtime, or a macOS-only Core ML path.

`Falconsai/text_summarization` is T5-small (60.5M parameters, Apache-2.0). Its
published configuration has a 512-token input window and a summarization output
limit of 200 tokens. The official repository publishes 830 MB of ONNX assets
and 310 MB of float32 Core ML assets. An unofficial GGUF repository publishes a
42.1 MB Q4_K_M quant and a 65.4 MB Q8_0 quant. Reusing Samosa's current native
runtime is therefore the smallest cross-platform deployment design.

Primary references:

- [Falconsai model and license](https://huggingface.co/Falconsai/text_summarization)
- [Published T5 configuration](https://huggingface.co/Falconsai/text_summarization/blob/main/onnx/config.json)
- [Published ONNX files](https://huggingface.co/Falconsai/text_summarization/tree/main/onnx)
- [Published Core ML files](https://huggingface.co/Falconsai/text_summarization/tree/main/coreml)
- [Third-party GGUF files](https://huggingface.co/mradermacher/text_summarization-GGUF/tree/main)

## Native validation performed

The 42.1 MB third-party Q4_K_M GGUF was downloaded to a temporary directory and
run with Samosa's installed Prism `llama.cpp` build `b9596-9fcaed76`. The model
loaded successfully, its prompt ran at about 5,057 tokens/second, and generation
ran at about 828 tokens/second. That proves the no-Python T5 execution path.

It did **not** prove the artifact is usable. For a short factual input, the
completion was corrupted (`dd dp syst ...`) instead of a summary. The tested
file's SHA-256 was:

```text
61f3a3f203a2c7eb13a90c641a8bde5993e2da43c4a8a528976484fb5aebee45
```

Do not add that third-party quant to the model catalog and do not place its
output in the evidence pipeline.

## Build and packaging path

1. Pin an exact revision of the official Falconsai safetensors and tokenizer.
2. In release CI, convert that pin with the same pinned Prism conversion code
   used by the shipped runtime, then quantize and record the final SHA-256.
   Python may be used by the release builder; it is not included in the app or
   invoked on a user's machine.
3. Test Q8_0 first because the model is already small and factual retention is
   more important than saving roughly 25 MB. Consider Q4_K_M only if it passes
   the same parity fixtures.
4. Add the verified GGUF as an auxiliary model-manager artifact whose runtime
   dependency is the existing `llama-server` package.
5. Start a small, loopback-only auxiliary `llama-server` lazily for long-text
   work. Use the completion endpoint with the T5 `summarize:` prefix; do not
   send T5 a chat template. Stop it after an idle timeout.

No shipped Python runtime is needed in this design.

## Where summarization belongs

Do not summarize search-result ranking input. Eight titles plus bounded
400-character excerpts are already small, and replacing them with a generated
summary would discard the strongest relevance signals.

For a fetched article or document longer than the decision prompt budget:

1. Split extracted text into sentence-aligned chunks of at most about 450 T5
   tokens, leaving room for the `summarize:` prefix and special tokens.
2. Summarize each chunk deterministically with a strict output cap.
3. Carry verbatim anchors beside the summary: the fetched title and source URL,
   plus bounded source sentences containing question terms, numbers, dates, or
   explicit negation.
4. Give the decision LLM the summaries and anchors, all labelled as untrusted
   web text.
5. Keep bounded raw fetched text as the evidence used for the final answer and
   citations. An abstractive summary is navigation, never the sole authority
   for a count, date, diagnosis, legal conclusion, or financial claim.

The 512-token window means this is a map/reduce summarizer, not a model that can
consume an entire long article in one pass. English-only content can use the
summarizer; unsupported languages must bypass it and use the existing raw-text
path.

## Required acceptance gates

- Native smoke test produces coherent text and exits cleanly with Python
  removed from `PATH`.
- Parity against the official Transformers implementation on a fixed fixture
  corpus, including the same tokenizer IDs and deterministic decoding.
- Exact-token preservation fixtures for names, locations, counts, dates,
  units, and negation.
- The transcript regression: a generic national page must not gain a South
  Carolina count, while a state page must retain its state/count evidence.
- Multi-chunk ordering, overlap, truncation, prompt-injection, and malformed
  Unicode fixtures.
- A missing, crashed, timed-out, or low-quality summarizer must fall back to
  bounded raw text; it must never block web research.
- Measure startup time, peak RSS, CPU/Metal contention with every supported
  chat backend, and end-to-end latency before enabling it by default.

The model card's reported evaluation values look like placeholders (for
example, repeating decimal sequences and round synthetic throughput figures),
so they are not sufficient release evidence. Samosa needs its own fixtures and
parity results before this model can influence decisions.

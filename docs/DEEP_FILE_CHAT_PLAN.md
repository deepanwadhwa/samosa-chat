# Deep file chat implementation plan

Status: planning only

Branch: `codex-deep-file-chat-plan`

Prepared: 2026-08-20

## Outcome

An individually attached file becomes durable, verbatim source material for a
conversation. A user can ask detailed follow-up questions, close and reopen the
app, and continue asking about the same file without attaching it again.

The single-file answer path must never replace source text with a generated
summary. A summary is allowed only when the user explicitly asks for one, or as
optional display metadata; it is never the sole evidence given to the answering
model.

This applies to:

- PDFs, including the existing text-layer/OCR path;
- UTF-8 text-like files regardless of extension: `.txt`, `.md`, source code,
  shell scripts, JSON, YAML, TOML, XML, CSV, logs, and similar files;
- `.docx` after its bounded ZIP/XML extractor lands.

Legacy binary `.doc`, RTF, archives, executables, and unknown binary formats are
not silently treated as text. They receive a specific unsupported-format error.

## Why the current behavior cannot answer deep questions

The shipped attachment flow is lossy in several independent places:

1. `assets/app.html` restricts the document picker to PDF.
2. `attachments_post_handler()` in `src/samosa_gateway.c` accepts only images
   and PDF, even though `samosa-extract --json` already reads valid UTF-8 text.
3. `attachment_augment()` summarizes any extracted document longer than 1,400
   characters. It sends a generated digest and at most a 6,000-character opening
   excerpt. Details later in the file are absent from the model prompt.
4. The browser sends `attachment_ids` only on the turn where the file was
   selected. Qwen can retain that turn in `session.qws`, but stateless backends
   do not receive the file on later questions.
5. Automatic Qwen session compaction summarizes old context. Today there is no
   separate document manifest from which exact file evidence can be restored.
6. The attachment test checks only that some extracted document marker reached
   the fake backend. It does not prove that late-file facts survive, follow-ups
   remain grounded, or compaction preserves the document.

Removing the native summarizer call fixes only item 3. The complete feature
requires canonical extracted text, a conversation/document relationship, a
context-aware full-versus-retrieval decision, and document-aware compaction.

## Product rules

1. **Canonical evidence is immutable and verbatim.** Store the uploaded bytes,
   extracted text, structure markers, and extraction fingerprint. Generated text
   never overwrites them.
2. **Documents are conversation-scoped after send.** Images may remain
   turn-scoped. A document chip should say that it remains available throughout
   the chat and offer an explicit detach action.
3. **Use the whole file when it safely fits.** Prefill it once into Qwen's
   durable session and preserve it through compaction. Stateless backends may
   receive it again on each request because they have no reusable KV session.
4. **Retrieve verbatim chunks when the whole file does not safely fit.** Do not
   silently truncate or summarize an oversized file.
5. **Cite the evidence actually used.** PDF answers cite page(s); text, code,
   and structured-data answers cite line ranges, with section/symbol metadata
   when available.
6. **Absence is an answer.** If retrieval finds no supporting passage, instruct
   the model to say that the answer was not found in the attached file.
7. **File contents are untrusted data.** Literal chat/control token spellings or
   instructions inside a file remain inside delimited data blocks and cannot
   become system instructions.
8. **No hidden mode changes.** The API and UI expose `full` or `retrieval`, exact
   token count, extraction limitations, and the reason retrieval was selected.

## Architecture

```text
file picker
    -> POST /v1/attachments (raw bytes, content-addressed)
    -> type sniff + bounded extraction + exact token count
    -> immutable extraction cache keyed by bytes + extractor fingerprint
    -> bind attachment ID to conversation documents.json
    -> choose full or retrieval from the effective context budget
       -> full: exact source prefilled once and pinned across compaction
       -> retrieval: exact chunks selected for every question
    -> answer + structured file/page/line citations
```

### 1. Type detection and extraction

Extend attachment sniffing in this order:

- known image magic;
- PDF magic;
- ZIP/DOCX magic plus required DOCX entries;
- valid text encoding with no NUL/control-binary signature;
- otherwise unsupported binary.

Do not trust filename or browser MIME type for capability decisions. Keep both
as display hints only.

For text-like files, preserve bytes as text after deterministic newline and
encoding normalization. Do not parse JSON, strip Markdown, or remove source
comments; those details are often exactly what the user wants to discuss.
Record line-start offsets so citations map back to the visible file. A future
UTF-16 BOM conversion may be added explicitly; invalid or ambiguous encodings
must fail clearly rather than produce replacement-character garbage.

For PDF, retain page boundaries and the source label already emitted by
`doc.read` (`text_layer`, `text_layer_ocr_unavailable`, or `ocr`). Preserve OCR
confidence/review fields with each page.

For DOCX, implement the already-decided portable path from
`docs/TASKS_DOCUMENTS.md`: bounded ZIP extraction plus XML text parsing. Enforce
compressed size, expanded size, entry count, path, and compression-ratio caps.
Legacy `.doc` is a different binary format and remains unsupported in this
work.

Extraction output should have one versioned shape for every format:

```json
{
  "schema": "samosa.document.v1",
  "attachment_id": "sha256",
  "extractor_fingerprint": "...",
  "media_type": "text/x-python",
  "filename": "worker.py",
  "text": "verbatim normalized text",
  "tokens": 4312,
  "segments": [
    {"ordinal": 0, "page": null, "line_start": 1, "line_end": 87,
     "start_offset": 0, "end_offset": 4096, "text": "..."}
  ],
  "limitations": []
}
```

The extraction cache is global and content-addressed. A conversation stores a
reference to it, not another copy of the raw file or text.

### 2. Durable conversation document manifest

Add `~/.samosa/chats/<conversation-id>/documents.json` beside `metadata.json`
and `session.qws`:

```json
{
  "schema_version": 1,
  "documents": [
    {
      "attachment_id": "sha256",
      "filename": "worker.py",
      "extractor_fingerprint": "...",
      "tokens": 4312,
      "mode": "full",
      "added_at": "..."
    }
  ]
}
```

Publish it with the repository's existing temp-write, `fsync`, and atomic
rename pattern. An `attachment_ids` value on a conversation-bound chat request
adds documents to this manifest before model admission. Later requests load the
manifest automatically, so old browser tabs and non-Qwen backends behave
consistently.

Add authenticated conversation routes:

```text
GET    /v1/conversations/<id>/documents
DELETE /v1/conversations/<id>/documents/<attachment-id>
```

The existing upload and chat APIs remain compatible. A stateless request with
no `conversation_id` keeps one-turn attachment behavior.

Detaching removes the conversation reference and retrieval state, not the
original user file. Content-addressed blobs follow existing reference/garbage
collection policy.

### 3. Full-file mode

Use exact tokenizer counts, not characters or megabytes. Select full mode only
when all pinned documents plus trusted prompt framing fit with room for:

- the current saved session;
- the user's question;
- requested output tokens;
- compaction memory and a conservative safety margin.

The implementation should begin with a configurable conservative threshold
(for example, pinned documents at most 60% of effective context) and replace it
with a measured threshold after the long-context gate. The response reports the
actual calculation and selected mode.

On the initial Qwen turn, send the exact document in a separately delimited,
untrusted block and save it into `session.qws`. Follow-ups should not prefill the
same document again.

Extend the Qwen request/session contract with a pinned-context ID and exact
pinned content. On ordinary resume the engine recognizes that the pinned ID is
already in the session and does not append it. If automatic compaction runs, the
engine rebuilds the compacted session with the exact pinned document block plus
the compact conversation memory and recent tail. Compaction may summarize the
conversation, but never the pinned file.

If the pinned block can no longer fit safely, stop before mutation and switch
the conversation document to retrieval mode (or return a specific error until
that transition is implemented). Never compact the only exact copy away.

### 4. Retrieval mode

Create deterministic chunks of roughly 800 model tokens with roughly 120-token
overlap, preferring page, paragraph, function/class, and line boundaries. Each
chunk keeps attachment ID, filename, page/line anchors, offsets, token count,
and extraction fingerprint.

Use local lexical retrieval; no embedding model exists. Reuse the maintained
Chutni lexical retrieval contract where its semantics fit rather than reviving
the removed private `samosa_chutni_db` implementation. Keep individual
attachment indexes separate from folder-memory summaries.

Ranking should combine:

- BM25 terms from the current question;
- exact quoted strings, JSON keys, paths, and code identifiers;
- symbol/heading matches;
- adjacent-chunk expansion for continuity;
- the last one or two user questions for referential follow-ups such as “that
  function,” without treating previous assistant claims as evidence.

Inject the top chunks within a strict token budget and emit the exact selected
chunks as citation metadata before answer text. Retrieval blocks contain no
generated summaries. General overview requests may deliberately cover more
chunks, but still use bounded verbatim evidence; an explicitly requested final
summary is model output, not stored source.

### 5. UI behavior

- Change the file picker from PDF-only to the server-reported supported file
  matrix. The picker hint is convenience; the gateway remains authoritative.
- Show `Reading…`, exact token count, `Full file in this chat` or `Searching
  relevant passages`, and extraction errors on the document chip.
- Keep conversation-bound document chips visible near the composer on later
  turns and after reload.
- Render PDF page citations and text/code line citations as file chips. Opening
  a citation can be a later enhancement; the first version must at least show
  exact anchors and the quoted supporting passage.
- Do not say a file was fully read when only retrieved chunks were used.
- Do not label background extraction or indexing as model “thinking.”

## Delivery phases

### Phase 0 — Capture the regression

- Add a text fixture whose decisive facts appear after both the current
  1,400-character summary threshold and 6,000-character opening excerpt.
- Make the fake summarizer omit those facts.
- Prove the current attachment request cannot see the late-file sentinel.
- Add a second-turn test without `attachment_ids` and a compaction/restart arm.

Exit: tests fail for the current lossy behavior for the intended reasons.

### Phase 1 — Verbatim PDF and text attachments

- Accept valid text-like uploads and retain their safe display names/extensions.
- Route text attachments through `samosa-extract --json` rather than the image
  OCR branch in `doc_read_handler()`.
- Remove document summarization/truncation from `attachment_augment()`.
- Add exact token count and a clear `document_too_large_for_full_context` error
  until retrieval mode is available.
- Update the picker and capability response.

Exit: a PDF, Markdown file, Python file, shell script, and JSON file arrive at
the fake backend byte-for-byte after documented newline normalization; late
sentinels remain present; oversized input is refused rather than summarized.

### Phase 2 — Conversation binding and pinned Qwen sessions

- Add the durable document manifest and list/detach routes.
- Automatically apply bound documents on later turns for stateless backends.
- Add Qwen pinned-context deduplication and exact preservation during session
  compaction.
- Restore document chips from server state after browser/app restart.

Exit: follow-ups work without reattachment, Qwen does not re-prefill the file
on every turn, and exact late-file facts survive restart and forced compaction.

### Phase 3 — Oversized-file retrieval and citations

- Build the deterministic chunk index and lexical/code-aware ranking.
- Select retrieval from exact context math instead of a hard character limit.
- Add structured document citation SSE/events and UI rendering.
- Add explicit not-found grounding instructions and tests.

Exit: a 100-page PDF and an oversized source file answer deep questions from
late sections with correct page/line citations, without any hidden summary or
silent truncation.

### Phase 4 — DOCX and hardening

- Add bounded DOCX ZIP/XML extraction and the adversarial extraction corpus.
- Complete format/capability copy and installer packaging.
- Run long-context, performance, memory, and real-model gates.

Exit: the supported-format matrix is accurate on each tested platform; malformed,
encrypted, oversized, deceptive, and hostile inputs fail specifically and do
not crash the gateway or resident model.

## Verification matrix

### Cheap automated gates

- Upload sniffing: PDF, UTF-8 text with an unknown extension, misleading MIME
  type/extension, NUL-containing binary, empty file, and size limit.
- Exactness: beginning/middle/end sentinels and significant code/JSON
  whitespace survive extraction and prompt construction.
- Injection: Qwen control-token spellings and “ignore previous instructions”
  inside a file remain quoted untrusted data.
- Persistence: first turn binds; second turn omits `attachment_ids`; restart
  still finds the document; detach removes it from later turns.
- Context boundaries: just-under budget chooses `full`; just-over chooses
  `retrieval`; neither silently truncates.
- Retrieval: late page, exact JSON key, Python symbol, shell flag, adjacent
  context, referential follow-up, and absent answer.
- Compaction: forced compaction preserves pinned full-file bytes or performs an
  explicit retrieval transition before replacing the session.
- Concurrency/crash safety: duplicate uploads, simultaneous binding, interrupted
  extraction/index publication, and stale cache fingerprint.
- Accessibility/UI: keyboard picker, progress/error announcement, persistent
  chip, detach action, and citations rendered through `textContent`.

### Real-model acceptance gates

Run these through the actual browser -> gateway -> installed local model path:

1. A roughly 6K-token, 10-page PDF: ten detailed questions, at least 8/10
   grounded, including facts near the end; follow-up TTFT measured separately
   from initial prefill; restart remains grounded.
2. A representative Python or shell file: architecture, control-flow, exact
   default/value, error path, and late-defined symbol questions with correct
   line citations.
3. A nested JSON file: exact keys/values and relationships, including repeated
   field names in different objects.
4. A 100-page PDF in retrieval mode: the existing target of at least 7/10
   grounded answers with correct page citations, plus an absent-answer refusal.
5. A forced Qwen compaction followed by questions whose answers occur only in
   the pinned document, not in recent chat turns.

Record command output, machine/OS/model details, wall time, prefill tokens/sec,
peak RSS, swap delta, and any skipped platform in
`docs/regressions/deep-file-chat/`. Unit tests or a successful build alone do
not satisfy these gates.

## Primary code touchpoints

- `assets/app.html`: picker accept rules, persistent document chips, mode and
  progress copy, detach, citation rendering, and follow-up request behavior.
- `src/samosa_gateway.c`: type sniffing, attachment metadata, extraction cache,
  conversation manifests/routes, exact augmentation, mode selection, retrieval,
  and citation events. Remove only the document use of the native summarizer;
  web and Chutni summarization are separate features.
- `src/samosa_extract.c`: normalized text contract, exact token counts, DOCX,
  page/line structure, errors, and extractor fingerprint.
- `src/qwen36b.c`: pinned-context identity, preflight accounting, session resume,
  and exact preservation during compaction.
- A factored document-index module: deterministic chunking and lexical indexing
  without coupling document attachments to folder-summary behavior.
- `tests/test_attachments.sh`, `tests/test_samosa_extract.sh`, the fake backend,
  UI contract tests, and new compaction/retrieval fixtures.
- `docs/SERVE_API.md`, `docs/USAGE.md`, and capability/installer documentation
  after behavior exists and has been verified.

## Risks and decisions to validate first

1. **Pinned compaction capacity.** Measure how much context must be reserved so
   exact document + compact memory + recent tail can always be rebuilt. This
   determines the full-mode threshold.
2. **Prefill UX.** Initial full-file ingestion can take minutes on the reference
   machine. The UI needs real token progress and must not look frozen.
3. **Code retrieval.** Plain BM25 is weak for punctuation-heavy identifiers and
   repeated JSON keys. The exact identifier/path boost is part of the first
   retrieval implementation, not optional polish.
4. **DOCX attack surface.** Do not ship DOCX by merely accepting ZIP files; the
   expansion limits and hostile corpus gate the feature.
5. **Multi-document pressure.** Compose full documents only while their combined
   pinned budget fits. Otherwise put one or more documents into retrieval mode
   and tell the user which mode each file uses.

## Definition of done

The feature is complete only when an attached supported file remains durable
verbatim evidence across follow-ups, restart, and compaction; oversized files
use cited verbatim retrieval; unsupported inputs fail clearly; and the real
installed model passes the acceptance gates. No answer path may depend on a
generated attachment summary or an unlabeled truncated excerpt.

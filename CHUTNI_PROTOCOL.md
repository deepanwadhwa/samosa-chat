# Chutni: Source-Bound AI Artifact Interchange

- **Document revision:** Foundation draft 0.2
- **Target profiles:** Chutni Core and Pack 0.1
- **Date:** 2026-07-26
- **Status:** Standalone proposal; no conformance claims yet
- **Project spelling:** Chutni

## How to read this document

- For the product idea and the honest go/no-go assessment, read Sections 1–7
  and 22.
- For the technical contract, read Sections 8–20.
- For a practical first release, read Sections 23–26.

## 1. Executive summary

Chutni is an open interoperability profile for exchanging immutable artifacts
derived from exact revisions of source material.

A Chutni pack can carry extracted text, OCR, transcripts, captions, summaries,
annotations, chunks, thumbnails, tables, and declared provenance. It tells a
receiving application:

- which exact source bytes an artifact came from;
- which other artifacts were used to create it;
- which process, software, model, recipe, or human produced it;
- where an excerpt or observation is anchored;
- how to verify every included payload; and
- which parts are durable evidence or interpretation versus disposable
  acceleration data.

This lets two unrelated AI applications reuse compatible work instead of
repeating parsing, OCR, transcription, captioning, or inference.

Chutni is independent of Samosa. Samosa may become one implementation, but no
normative Chutni requirement may depend on Samosa, a particular model, a
particular database, a particular operating system, or a particular transport.

An LLM does not itself “implement Chutni.” The application hosting the LLM
implements Chutni: it validates packs, verifies hashes, enforces permissions,
retrieves artifacts, and presents selected content to the model.

The central promise is deliberately narrower than “prepare once, use
everywhere”:

> Derive once, preserve the evidence, and reuse it wherever compatibility and
> local policy permit.

## 2. The brutally honest verdict

Chutni is realistic, but only in a narrow form.

Most of the mechanisms in the original proposal already exist. Packaging,
checksums, provenance graphs, annotations, content-addressed blobs, runtime
resource access, and archival versioning all have established standards.
Reinventing those pieces would make Chutni less credible, not more innovative.

Chutni should therefore be a **small interoperability profile built from proven
standards**, not a new database, archive format, provenance ontology, selector
language, search protocol, or universal AI-memory system.

At v0.1, Chutni is technically closer to a data model plus package profile than
an interactive protocol. That is not a weakness. “Chutni protocol” can name the
eventual family of exchange, binding, and gateway profiles without pretending
that filesystem watching, search calls, or permissions belong in its first
portable layer.

The defensible contribution is the combination of:

- exact, immutable revisions of local or user-controlled sources;
- reusable AI-derived artifacts bound to those exact revisions;
- precise attribution into text, pages, regions, sheets, bytes, or time ranges;
- declared processing provenance across parsers, models, applications, and
  humans;
- a hard separation between durable artifacts and disposable indexes;
- recipient-local source rebinding without portable filesystem authority; and
- a safe contract for using retrieved artifacts as untrusted model context.

None of these ideas is novel alone. The potentially useful and novel product is
the narrow integration contract among them.

The closest existing foundations are RO-Crate, W3C PROV, W3C Web Annotation,
and BagIt. A notable but young adjacent project is Google's Open Knowledge
Format (OKF) v0.2. Chutni only deserves to exist if it remains more precise and
lower-level than OKF: it should exchange machine-verifiable, revision-bound
derivatives, not compete as another directory of curated Markdown knowledge.

The hard test is simple:

> Can two independently written applications exchange an expensive derived
> artifact, verify its exact inputs and anchors, and reuse it without rerunning
> the work?

If the answer is no, Chutni is merely another metadata schema. If Chutni ships
as a bespoke SQLite catalog containing summaries and embeddings, adoption is
unlikely. If it ships as a small profile with excellent validators, malicious
test fixtures, and two independent implementations, it has a credible path.

## 3. Why Chutni exists

AI applications repeatedly perform the same expensive preparation:

1. discover a file;
2. identify its format;
3. extract or OCR its contents;
4. transcribe or caption media;
5. divide the result into useful selections;
6. summarize or annotate it;
7. build a search index; and
8. discard most of the lineage when the application is closed or replaced.

The result is wasteful and hard to trust:

- users pay the compute cost again in every application;
- applications retain outputs in incompatible private databases;
- a summary is separated from the exact source revision it describes;
- citations use ambiguous offsets or point only to a mutable filename;
- changing one file can invalidate an entire opaque index;
- imported paths may accidentally expose private filesystem structure;
- embeddings are treated as universal even when their compatibility is narrow;
  and
- users cannot easily move useful preprocessing with their data.

Chutni creates a portable boundary between **derivation** and **use**. A
producer can preserve a derived artifact with enough evidence for another
application to decide whether it is safe and useful to reuse. A consumer may
still reject the artifact, rebuild it, or reopen the source.

### 3.1 Why Chutni must be standalone

Samosa and Chutni have different responsibilities and release pressures.
Samosa may choose a model, local database, UI, search strategy, watcher,
gateway, and performance tradeoffs. A portable profile must remain neutral
across all of them.

A separate repository provides:

- neutral naming, governance, licensing, and issue tracking;
- a specification lifecycle independent of one application's releases;
- test fixtures that any implementation can use;
- room for adapters in other languages and storage engines;
- proof that the design works without Samosa internals; and
- a clear boundary between portable meaning and product behavior.

Samosa should consume the same public schemas and validator as everyone else.
It should not have a privileged private interpretation of a Chutni term.

## 4. What Chutni is

Chutni is:

- a logical model for sources, immutable source revisions, artifacts, payload
  blobs, processing activities, agents, plans, relations, and selectors;
- a constrained RO-Crate profile for serializing that model;
- a BagIt-based transfer profile for complete, verifiable packs;
- a set of security rules for import, local source binding, retrieval, and
  disclosure;
- a modular conformance system for producers, consumers, and optional
  bindings; and
- an interchange layer that an application can map into any suitable internal
  store.

Chutni is intended for source-derived material such as:

- local documents and files;
- source-controlled files;
- user-authorized object-store items;
- archive members;
- images, audio, and video;
- spreadsheets and structured exports; and
- multiple exact inputs used to create a combined artifact.

The original source remains authoritative for the bytes observed and for
statements attributed to it, not necessarily for factual truth. A verified
correction may be more accurate. A Chutni artifact is evidence, interpretation,
or acceleration derived from a source; it is not a silent replacement for the
source record.

## 5. What Chutni is not

Chutni is not:

| Not Chutni | Reason |
|---|---|
| A conversational or personal memory protocol | Identity, preferences, goals, chat history, and personality memory are a different and already crowded problem space. |
| A vector database or RAG framework | Search and ranking are consumer choices. Indexes are projections that may be rebuilt. |
| A universal model cache | Token IDs, KV caches, projector outputs, and most embeddings have narrow compatibility. |
| A source backup format | A pack may omit original source bytes and must say when it does. |
| A source-of-truth replacement | Exact or high-stakes claims should be checked against the source when available. |
| A synchronization protocol | Conflict resolution, replicated mutation, and deletion propagation are outside the core. |
| An access-control list | Authority and disclosure policy live in protected recipient-local state. |
| A filesystem capability | A locator never grants permission to open a path or URL. |
| A model-quality oracle | Producer names and confidence numbers do not establish correctness. |
| An execution format | Imported code, prompts, documents, and metadata are untrusted data. |
| A required SQLite schema | Applications may use SQLite, Postgres, files, an object store, or another internal representation. |
| A transport protocol | MCP, HTTP, local IPC, a library API, or a CLI may expose Chutni, but none defines its core meaning. |
| A new packaging or provenance standard | Chutni profiles RO-Crate, BagIt, PROV, and Web Annotation. |

Chutni also does not promise that every artifact is reusable by every model.
Portable UTF-8 text, OCR, transcripts, captions, tables, and citations are the
strongest common layer. Model-specific acceleration is optional and must fail
closed when compatibility is unknown.

## 6. Layered architecture

```text
                         Application / LLM host
                    search, policy, UI, model prompts
                                  |
                    optional Gateway or MCP binding
                                  |
           +---------------- Chutni Core ----------------+
           | SourceRevision -> Activity -> Artifact      |
           |       exact inputs, blobs, provenance,      |
           |       selectors, relations, integrity       |
           +---------------------------------------------+
                   |                              |
        Chutni Exchange Profile          recipient-local state
          RO-Crate + BagIt               bindings, observations,
                   |                     permissions, indexes
             .chutnipack
```

The layers are:

1. **Core semantic profile** — the portable meaning of sources, revisions,
   artifacts, activities, agents, plans, payloads, and selectors.
2. **Exchange profile** — an RO-Crate representation of a closed, immutable
   snapshot.
3. **Bag profile** — a complete BagIt directory containing the Exchange graph
   and content-addressed objects.
4. **Pack archive profile** — the safe ZIP64 serialization of a Chutni Bag,
   using the `.chutnipack` extension.
5. **Local binding profile** — recipient-local mappings from logical sources to
   authorized locations, plus the revision observed through each binding. These
   mappings are not normally exported.
6. **Candidate acceleration profile** — a future, compatibility-bound form for
   embeddings. Search indexes and runtime caches remain outside ordinary packs.
7. **Gateway profiles** — optional APIs such as MCP for controlled runtime
   access. These do not change the core data model.
8. **Implementation-private live store** — mutable jobs, watchers, locks,
   indexes, caches, and application state. This is not a portable format.

This separation is important. A whole-drive live catalog may contain millions
of records and high-performance indexes, while a transfer pack should usually
be a selective, immutable snapshot. Chutni should not force both workloads into
one physical layout.

## 7. Standards Chutni reuses

Chutni should standardize only the missing domain contract.

### 7.1 Normative foundations

| Concern | Existing standard | Chutni use |
|---|---|---|
| Metadata graph and domain profile | [RO-Crate 1.3](https://www.researchobject.org/ro-crate/specification/1.3/index.html) and [JSON-LD 1.1](https://www.w3.org/TR/json-ld11/) | The Chutni Exchange Profile is an RO-Crate profile. |
| Derivation and responsibility | [W3C PROV-DM](https://www.w3.org/TR/prov-dm/) / [PROV-O](https://www.w3.org/TR/prov-o/) | Sources and artifacts are entities; runs are activities; people and software are agents; recipes are plans. |
| Exact targets and selections | [W3C Web Annotation](https://www.w3.org/TR/annotation-model/) | Use `SpecificResource` and standard selectors rather than inventing generic offset JSON. |
| Audio, video, and image fragments | [W3C Media Fragments](https://www.w3.org/TR/media-frags/) | Reuse temporal and spatial fragment semantics. |
| Text and CSV fragments | [RFC 5147](https://www.rfc-editor.org/rfc/rfc5147) and [RFC 7111](https://www.rfc-editor.org/rfc/rfc7111) | Reuse when the media type and coordinate system match. |
| Complete transfer and fixity | [BagIt 1.0 / RFC 8493](https://www.rfc-editor.org/rfc/rfc8493) | Inventory every payload file and verify the required tag files; tag manifests do not list themselves. |
| IDs | [UUIDs / RFC 9562](https://www.rfc-editor.org/rfc/rfc9562) | UUIDv7 URNs are a recommended generated-identifier form, not the only form. |
| Times | [RFC 3339](https://www.rfc-editor.org/rfc/rfc3339) | Portable timestamps use an explicit UTC offset. |
| Media and language | [IANA media types](https://www.iana.org/assignments/media-types/media-types.xhtml) and [BCP 47](https://www.rfc-editor.org/rfc/bcp/bcp47.txt) | Describe payload encoding and natural language. |

### 7.2 Informative mappings and candidate bindings

| Concern | Existing work | Chutni use |
|---|---|---|
| Processing-run serialization | [Process Run Crate 0.5](https://www.researchobject.org/workflow-run-crate/profiles/process_run_crate/) | Publish a deterministic crosswalk for activities, inputs, outputs, software, and run agents before conformance. |
| Blob descriptor shape | [OCI Image Spec 1.1.1 descriptor](https://github.com/opencontainers/image-spec/blob/v1.1.1/descriptor.md) | Reuse the proven `mediaType`, `digest`, and `size` shape without claiming OCI conformance or requiring a registry. |
| JSON canonicalization | [RFC 8785](https://www.rfc-editor.org/rfc/rfc8785) | Use only when a Chutni profile needs canonical hash-bearing JSON. |
| Validation | [SHACL](https://www.w3.org/TR/shacl/) plus JSON Schema where useful | Validate the graph and syntax; semantic checks still require code. |
| Runtime model access | [MCP Resources](https://modelcontextprotocol.io/specification/2025-11-25/server/resources) | Optional read/search binding; MCP is not the store format. |
| Authenticated media provenance | [C2PA](https://spec.c2pa.org/) | Preserve and recognize existing Content Credentials; do not reinterpret them as proof that model output is true. |
| Archival repository layout | [OCFL 1.1](https://ocfl.io/1.1/spec/) | Optional long-term preservation binding, not core. |
| Curated agent knowledge | [Open Knowledge Format](https://github.com/GoogleCloudPlatform/knowledge-catalog/blob/main/okf/SPEC.md) | Optional human-readable projection from Chutni artifacts; not the canonical evidence graph. |

SHA-256 is mandatory for portable Chutni payload descriptors. An implementation
MAY attach BLAKE3 or SHA-512 as additional payload digests, but a Core consumer
is not required to use them. There is no store-wide `hash_algorithm`; each
descriptor names its algorithm.

BagIt has a separate tooling requirement: conforming BagIt creation and
validation tools support both SHA-256 and SHA-512. Chutni Bag readers and
writers therefore MUST implement both algorithms. A Chutni Bag MUST contain
the profile-required SHA-256 payload and tag manifests and MAY additionally
contain SHA-512 manifests.

Chutni must not invent cryptography. A future confidential or signed-pack
profile should reuse a reviewed envelope such as
[COSE](https://www.rfc-editor.org/rfc/rfc9052),
[DSSE](https://github.com/secure-systems-lab/dsse), or an applicable C2PA
mechanism after its threat model and key-management rules are specified. Until
then, transport or storage encryption may protect packs, but it is not a Chutni
conformance claim.

Process Run Crate is valuable prior art with multiple implementations, but its
current profile is pre-1.0 and was developed against earlier RO-Crate versions.
The Chutni wire profile MUST publish a deterministic mapping—potentially
dual-typing PROV Activities as Schema.org `CreateAction` records and mapping
`used`/generated outputs to `object`/`result`—before claiming compatibility. It
should not make Process Run Crate an unexamined hard dependency.

## 8. Normative language and document status

The key words **MUST**, **MUST NOT**, **REQUIRED**, **SHOULD**, **SHOULD NOT**,
and **MAY** in this document are to be interpreted as described by BCP 14
([RFC 2119](https://www.rfc-editor.org/rfc/rfc2119) and
[RFC 8174](https://www.rfc-editor.org/rfc/rfc8174)) when, and only when, they
appear in all capitals.

This foundation draft fixes Chutni's scope, model, layering, and intended
requirements. It is not yet sufficient for a production conformance claim. A
release candidate must also publish:

- a stable namespace and profile URI;
- the RO-Crate term mapping;
- a fixed JSON syntax schema, SHACL graph shapes, and procedural semantic
  checks, with the authoritative role of each documented;
- media-type and artifact-kind registries or extension rules;
- golden and malicious fixtures;
- a validator with reproducible results; and
- an interoperability report from at least two independent implementations.

The overview, comparisons, recommendations, examples, and roadmap are
informative. Sections that use uppercase requirement terms state the intended
normative design for the future profile.

## 9. Core invariants

A conforming Chutni design preserves these rules:

1. **Sources have logical identity; revisions identify exact source states.** A
   path is a locator, not a source ID. A digest verifies content but does not by
   itself globally identify a source-specific revision.
2. **Source revisions are immutable.** Different bytes require a different
   revision.
3. **Artifacts are immutable.** A correction or regeneration creates another
   artifact and a relation; it does not overwrite history.
4. **Artifacts target exact entities.** A content-derived artifact MUST have a
   direct or transitive provenance path to one or more exact source revisions.
   It MUST NOT be bound only to a mutable logical source.
5. **R1 remains R1.** If a source changes from R1 to R2, artifacts derived from
   R1 remain immutably bound to R1. Their accuracy or trust is unchanged and is
   never guaranteed by that binding.
6. **There is no portable `active` or `stale` flag.** “Current” is a
   recipient-local comparison between an artifact's revision and the revision
   observed through a particular local binding.
7. **Known material inputs are represented.** Every material input controlled
   or known by the producer MUST be recorded. Unknown or unavailable hosted
   model dependencies MUST be declared as such. Multi-source and
   multi-artifact derivations MUST NOT be forced onto a fake single source.
8. **Every selector names its coordinate space.** A selector MUST target one
   exact immutable revision or artifact and MUST NOT be silently applied to
   another representation.
9. **Payloads are verifiable.** Every portable artifact payload has a media
   type, byte length, and SHA-256 digest.
10. **Provenance is per artifact.** A store-wide producer label is insufficient.
11. **Declared provenance is not authenticated provenance.** A string naming a
    model, human, or organization is a claim unless an external trust mechanism
    authenticates it.
12. **Portable metadata grants no authority.** Paths, URLs, policies, and
    producer claims in a pack are untrusted data.
13. **Original sources preserve primary evidence.** They are authoritative for
    the observed bytes and attributed statements, not automatically for factual
    truth. A consumer decides when an artifact is sufficient and when the
    source must be opened.
14. **Indexes are disposable.** A recipient may discard and rebuild all search
    projections without changing the portable meaning.
15. **Retrieval is not instruction.** Source content and every derived string
    remain untrusted input when presented to an LLM or UI.

## 10. Conceptual model

### 10.1 Source

A `Source` is the stable logical identity of material across observations.
Examples include “the quarterly report maintained by this user” or “the
recording of this meeting.”

A Source is a conceptual `prov:Entity`. Each SourceRevision is another
`prov:Entity` that specializes the Source.

A Source:

- has an opaque identifier;
- may have a human-readable name and kind;
- may be associated locally with one or more locators;
- may have multiple immutable revisions; and
- is not identified by a pathname alone.

The continuity decision is semantic. If unrelated bytes replace a file at the
same path, an application SHOULD create a new Source rather than infer that the
new file is a revision of the old one. When continuity is uncertain, the
application should preserve both possibilities or ask the user.

Portable source metadata SHOULD avoid machine-specific identifiers. Absolute
paths, usernames, device and inode IDs, volume labels, share names, access
tokens, and credentials MUST be omitted from ordinary exports.

### 10.2 SourceRevision

A `SourceRevision` is an immutable observation of exact source bytes. It is a
`prov:Entity` and a specialization of a logical Source.

A SourceRevision records:

- its own identifier;
- the logical Source it specializes;
- an OCI-shaped descriptor with media type, digest, and byte length;
- an observation or generation time when known;
- optional safe descriptive metadata such as language;
- whether its source bytes are included in the pack; and
- optional lineage to a prior revision when that relationship is known.

The SHA-256 digest is the portable verification value for the observed bytes;
it is not the SourceRevision's global identity by itself. File modification
time, size alone, an inode, an ETag without defined semantics, or a path is not
sufficient.

Changing only a path, modification time, permissions, or other binding metadata
does not create R2 when the bytes are identical. Changing the bytes creates a
new SourceRevision. Two logical Sources may still refer to distinct
SourceRevisions whose payload descriptors happen to contain the same digest.

`prov:wasRevisionOf` SHOULD be asserted only when R2 is genuinely a later
revision of R1. Both may still use `prov:specializationOf` to relate to the same
logical Source. A different file placed at the same path is not automatically a
semantic revision.

Core v0.1 assumes an exact byte-addressable resource. A directory revision
requires a separately specified, deterministic member-manifest algorithm.
Until such a profile exists, a directory summary is a multi-input Artifact over
the exact revisions of its included members, not a vaguely hashed directory.

### 10.3 Blob

A `Blob` is a sequence of payload bytes addressed by digest. The logical
descriptor shape is:

```json
{
  "mediaType": "text/plain; charset=utf-8",
  "digest": "sha256:0123456789abcdef...",
  "size": 18432
}
```

Following the OCI descriptor convention, `digest` and `size` cover the exact
raw byte sequence referenced by the descriptor. ZIP transport compression is
outside the descriptor and does not change those bytes. A profile that stores a
separately compressed object must give that compressed byte sequence its own
media type and descriptor rather than hash an abstract “logical” payload.

Different Artifact records may refer to the same Blob. Shared bytes do not
collapse distinct provenance.

### 10.4 Artifact

An `Artifact` is an immutable `prov:Entity` produced from one or more exact
revisions or artifacts. It has:

- an opaque identifier;
- a typed artifact kind;
- exactly one payload Blob;
- one or more exact Targets that state what it describes or annotates;
- the Activity that generated it;
- its media type and language where applicable;
- zero or more selectors or annotations;
- optional rights and sensitivity declarations; and
- typed relations to earlier or alternative artifacts.

Initial portable artifact kinds should be deliberately small:

| Family | Example kinds | Reuse expectation |
|---|---|---|
| Representation | extracted text, OCR text, transcript, normalized table | Strong when media type and coordinate semantics are supported. |
| Selection | chunk, excerpt, key frame, page image, table region | Strong when its exact target and selector are supported. |
| Interpretation | caption, summary, entity annotation, topic annotation | Reusable but advisory; consumers should inspect provenance. |
| Human contribution | correction, label, note, verification result | Reusable with declared authorship; not automatically authenticated. |
| Acceleration | embedding | Optional and compatibility-bound. |

The core should not define an artifact kind for every model task. Kinds use
stable IRIs, and extensions use namespaced IRIs. Unknown kinds may be retained
within local quotas but MUST NOT be treated as understood.

An Artifact has exactly one logical payload descriptor. Tiny text may still be
stored inline by a future serialization profile, but it must have the same
media type, byte length, and digest as an external Blob. A profile must never
create two competing payloads for one Artifact. Assertions with no content are
relations or Activity metadata, not empty Artifacts.

#### Target versus derivation input

A Target is an exact SourceRevision or Artifact plus an optional Web Annotation
selector. It defines what an Artifact describes, represents, corrects, or
annotates.

Derivation inputs belong on the generating Activity as `prov:used` or qualified
Usage edges. The Artifact links to that Activity with `prov:wasGeneratedBy` and
MAY carry `prov:wasDerivedFrom` shortcuts when they do not lose input roles or
ordering. For example, a summary may target SourceRevision R1 while its Activity
used an extracted-text Artifact and a style-guide Artifact.

### 10.5 Activity

An `Activity` is a `prov:Activity` representing the work that used exact inputs
and generated artifacts. Examples include parsing, OCR, transcription,
captioning, summarization, human correction, validation, or format conversion.

An Activity SHOULD record, when applicable:

- its identifier and type;
- all producer-controlled or known material inputs;
- start and end times;
- the performing software or human Agent;
- the model, parser, or executable revision;
- the Plan or recipe;
- relevant parameters and preprocessing;
- locale, determinism, or hardware details when materially relevant; and
- a result state.

The application, parser, and model are not the same thing. A useful provenance
record might say:

- application Agent `AcmeIndexer 2.4` performed the Activity;
- parser Entity `pdfium build abc123` was used;
- model Entity `vendor/model@revision` was used;
- Plan `recipe:ocr-and-layout/v3` governed the Activity; and
- SourceRevision R1 was the input.

A model's marketing name alone is not reproducibility metadata. For an API
model with an undisclosed moving implementation, the producer should record the
provider, requested model identifier, returned revision or fingerprint when
available, request parameters, recipe digest, and the fact that exact
reproduction may be impossible. Undisclosed system prompts, routing, weights,
safety layers, or other unavailable dependencies are explicitly marked
unknown; they are not silently presented as a complete recipe.

A producer MUST hash the exact byte stream or immutable snapshot supplied to
the Activity. It may copy the bytes, retain an immutable handle, or hash while
reading. Rehashing a pathname afterward is only an additional race check and
cannot prove which bytes were actually processed.

A failure is an Activity outcome or local job record, not a bogus content
Artifact.

### 10.6 Agent and Plan

An `Agent` is a `prov:Agent`: a person, software agent, or organization
responsible for an Activity or assertion.

A `Plan` is a `prov:Plan` describing how an Activity was intended to run. It may
identify a versioned recipe, prompt template, parser configuration, or
transformation contract. A Plan may be:

- included as a digest-bound declarative payload;
- referenced by a stable public identifier;
- summarized with its exact bytes withheld for privacy; or
- marked unavailable.

Prompt text and configuration frequently contain secrets. Exporters MUST not
leak credentials, private paths, hidden system instructions, or user content
through Plan metadata.

### 10.7 Input edges and relations

All producer-controlled or known material inputs MUST be explicit; unavailable
dependencies MUST be declared. Basic derivation uses
`prov:wasDerivedFrom`; the generating Activity uses `prov:used` and
`prov:wasGeneratedBy`.

When role, ordering, or selection matters, the profile uses qualified PROV
Usage with a role and, where necessary, a position. Examples include:

- primary document versus style guide;
- question versus evidence passages;
- ordered transcript segments;
- image plus OCR text; and
- five exact documents used for a synthesis.

Chutni-specific relations should be added only when PROV, Schema.org, and Web
Annotation cannot express the required meaning. Alternative representations
should first use `prov:alternateOf` or an applicable Schema.org encoding
relation. Corrections should first assess `prov:wasRevisionOf` and Web
Annotation's `editing` motivation. A purpose-qualified preference may still
require a Chutni term such as `supersedesForPurpose`. These relations never
mutate or invalidate the earlier Artifact.

### 10.8 ExchangeSnapshot

An `ExchangeSnapshot` is an immutable, selective graph represented by the
RO-Crate root Dataset and, where useful, a `prov:Bundle`. It identifies:

- the exact Chutni and RO-Crate profiles used;
- a new snapshot identifier;
- its creation time and declared exporter;
- the selected entities and required provenance closure; and
- whether original source bytes are attached or merely referenced.

Two snapshots may contain overlapping entities. Import does not mean blindly
appending one database to another: the recipient validates immutable identity,
detects conflicts, preserves origin, and applies local retention policy.

## 11. The R1/R2 revision model

Chutni does not make an Artifact intrinsically active or stale.

```text
Logical Source S1 ("Quarterly report")
    |
    +-- SourceRevision R1 --extract E1--> Artifact A1 (text)
    |                              |
    |                              +--summarize M1--> Artifact A2 (summary)
    |
    +-- SourceRevision R2 --extract E2--> Artifact A3 (text)
                                   |
                                   +--summarize M2--> Artifact A4 (summary)
```

R1 and R2 are immutable. If continuity is known, R2 may declare
`prov:wasRevisionOf R1`. A1 and A2 remain immutably and correctly bound to the
exact R1 lineage. Their factual accuracy is neither improved nor guaranteed by
that binding. They are never rewritten to refer to R2.

On one recipient's machine, protected local state may say:

```text
binding(B1, S1) = /authorized/project/quarterly-report.pdf
observed_revision(B1) = R2
```

That recipient can prefer the R2 lineage for queries using binding B1 and
present A2 as “derived from an earlier revision observed through this binding.”
Another authorized binding may expose R1 or a branch, and another recipient may
have no local source at all. The portable Artifact cannot correctly contain a
universal currentness flag for all of them.

Useful local resolution states include:

- **unbound** — no authorized local candidate is associated with the Source;
- **digest matched** — a local candidate was verified as a particular revision;
- **different revision observed** — a candidate reached through that binding
  now hashes to another revision; and
- **source unavailable** — a previous binding cannot currently be resolved.

These are recipient observations, not immutable properties of the Artifact.

## 12. Selectors and exact anchoring

A selector describes a part of an exact target. Every selector MUST identify:

1. the immutable target Revision or Artifact;
2. the selector type;
3. the coordinate system required by that selector; and
4. enough information to interpret and structurally validate it.

Structural validation checks syntax, non-negative ordered positions, declared
coordinate semantics, and immutable target identity. Resolved validation checks
bounds, quotes, pages, regions, or time ranges against target bytes. A consumer
MUST perform resolved validation when the target payload is available. When the
source-referenced target is intentionally detached, it MUST report the selector
as unresolved or unverified rather than treating the whole pack as invalid.

Chutni reuses Web Annotation selectors:

- `TextPositionSelector` for zero-based, half-open Unicode code-point offsets in
  a defined text representation;
- `TextQuoteSelector` for quoted text with optional prefix and suffix;
- `DataPositionSelector` for byte positions in a binary representation;
- `FragmentSelector` for registered media fragments;
- `SvgSelector` for spatial regions when appropriate; and
- `RangeSelector` or refined selectors for compound locations.

Text selectors SHOULD include both quote and position information when feasible.
The quote helps detect drift or coordinate mistakes; the position permits
efficient access.

For a Chutni UTF-8 text Artifact, positions count Unicode code points in logical
order from the exact decoded Artifact bytes, with an inclusive start and
exclusive end. Selections SHOULD NOT split a grapheme cluster. Any HTML removal,
entity decoding, Unicode normalization, whitespace rewriting, or other text
normalization MUST occur in the generating Activity, be described by its Plan,
and be materialized in the target Artifact. Consumers MUST NOT apply a second,
undocumented normalization before resolving positions.

Coordinates never float between representations. Character offsets in an
extracted UTF-8 Artifact target that Artifact, not the original PDF revision.
Byte offsets in a PDF target the PDF revision, not its extracted text. A page
image region targets the exact page-image Artifact. A consumer MUST NOT silently
apply an R1 selector to R2 even when the files appear similar.

Use W3C Media Fragments for compatible temporal and spatial media selection,
RFC 5147 for `text/plain` fragments where appropriate, and RFC 7111 for CSV
fragments. Page, sheet, slide, cell, DOM, PDF-structure, or archive-member
selectors should reuse an existing stable selector vocabulary when one exists.
A Chutni-specific selector is justified only after documenting the gap.
There is no equally universal selector for an XLSX A1 range, for example, so
spreadsheet-native coordinates belong in an optional, precisely specified
profile rather than a pretend-generic Core selector.

## 13. Exchange, Bag, and Pack profiles

### 13.1 Exchange graph

The canonical exchange metadata is a Chutni profile of RO-Crate 1.3. The
RO-Crate JSON-LD graph contains the exported closure of:

- Sources;
- SourceRevisions;
- Artifacts and payload descriptors;
- Activities;
- Agents;
- Plans;
- typed input and derivation relations;
- selectors and annotations;
- the ExchangeSnapshot entity; and
- the Chutni and RO-Crate profiles to which the pack claims conformance.

For every selected Artifact, the required provenance closure contains:

1. the Artifact record, its descriptor, and its payload bytes;
2. every Target and selector;
3. its generating Activity;
4. every known direct input and qualified Usage edge;
5. the minimal descriptors of involved Agents, model/parser Entities, and
   Plans;
6. recursively, the same metadata for input Artifacts until at least one exact
   SourceRevision is reached; and
7. the logical Source for every included SourceRevision.

Original SourceRevision bytes and private Plan bytes MAY be detached. Their
entities, descriptors, and explicit availability state remain in the graph.
Public contextual IRIs may remain external, but a consumer is never required or
permitted to dereference them automatically. An exporter MUST NOT call a graph
closed if interpretation of a selected Artifact depends on an undeclared
required profile or a missing Chutni entity.

An export is a snapshot. Editing its graph or payload creates a new
ExchangeSnapshot with a new identifier. A later snapshot may relate to an
earlier one using PROV, but no receiver is required to apply it as a mutation.

Original source bytes are optional. If omitted, the pack is
**source-referenced**, not a source backup. If included, they are normal
digest-verified payload entities and are subject to explicit export consent.

### 13.2 Chutni Bag directory

The canonical directory package is a **Chutni Bag**. The example directory
suffix is informative; `.chutnipack` is reserved for the archive in
Section 13.3.

```text
Example.chutni-bag/
├── bagit.txt
├── bag-info.txt
├── manifest-sha256.txt
├── tagmanifest-sha256.txt
└── data/
    ├── ro-crate-metadata.json
    └── objects/
        └── sha256/
            └── 01/
                └── 0123456789abcdef...
```

`data/` is the root of the attached RO-Crate. BagIt manifests provide transfer
completeness and fixity. Payload objects are stored at paths derived from
strictly parsed digests; object paths are not arbitrary metadata fields.

A Chutni Bag has these v0.1 rules:

- `bagit.txt` MUST contain BagIt version 1.0 and declare UTF-8 tag-file
  encoding, exactly as `BagIt-Version: 1.0` and
  `Tag-File-Character-Encoding: UTF-8`, each followed by LF.
- `bag-info.txt` MUST use the ExchangeSnapshot identifier as its
  `External-Identifier` and contain `Bagging-Date`; it SHOULD identify the
  writing software and payload octet count using standard BagIt tags.
- `manifest-sha256.txt` MUST list every regular file below `data/` exactly once,
  including `data/ro-crate-metadata.json`.
- `tagmanifest-sha256.txt` MUST list `bagit.txt`, `bag-info.txt`,
  `manifest-sha256.txt`, and every other allowed non-tag-manifest tag file
  exactly once. In accordance with BagIt, no tag manifest lists itself or
  another tag manifest.
- Each SHA-256 manifest line is 64 lowercase hexadecimal characters, two ASCII
  spaces, the path using `/`, and LF. Entries are sorted by ascending ASCII
  path. Paths contain no escaping because Core paths are ASCII. These canonical
  restrictions make independent output predictable without changing BagIt
  semantics.
- `fetch.txt` and holey bags MUST NOT appear. A Core Chutni Bag is complete and
  never triggers external retrieval.
- Core paths are ASCII and are limited to the four required tag files,
  optional paired SHA-512 manifests, `data/ro-crate-metadata.json`, and
  `data/objects/sha256/<first-two-hex>/<full-64-lowercase-hex>`. Any other path
  requires a declared, supported extension profile.
- Every object below `data/objects/` MUST be referenced by exactly one or more
  descriptors in the exchange graph. Unreferenced payloads are forbidden.
- Every Artifact Blob in the graph MUST be included. SourceRevision and private
  Plan bytes MAY be detached only when their graph records say so.

Consumers MUST derive an object's sole valid path from its strictly parsed
SHA-256 digest. The shard is the first two hexadecimal characters and the
filename is the full 64-character hexadecimal digest.

BagIt detects incompleteness and accidental or unauthorized byte changes
relative to its manifests; it does not authenticate the sender. A malicious
sender can change content and recompute every manifest. Until a separate signed
profile exists, a Chutni Bag is unsigned and plaintext and MUST NOT be presented
as authenticated, signed, or confidential. Reuse of one ExchangeSnapshot
identifier with a different verified inventory is an immutable-identity
conflict and MUST be rejected or quarantined.

The following MUST NOT appear in a Chutni Bag:

- a live `catalog.sqlite`;
- SQLite WAL, SHM, journal, or lock files;
- mutable job, watcher, or queue state;
- temporary files, logs, crash dumps, or backups;
- executable language-native serialization;
- application search indexes; or
- token caches, model KV caches, or undocumented binary state.

An application imports the validated graph into its own clean store. It MUST
NOT replace its operational database with a foreign SQLite file.

### 13.3 Chutni Pack archive

BagIt defines a directory package, not ZIP. A v0.1 **Chutni Pack** is a ZIP64
serialization of a Chutni Bag and uses the `.chutnipack` extension. Archive
entries contain the bag contents directly: `bagit.txt` is at the archive root,
not below a wrapper directory. The archive MUST use only stored or Deflate
compression methods and UTF-8 entry names. Core entry names are ASCII.

Until a Chutni media type is registered, senders use `application/zip` plus the
`.chutnipack` filename convention. An importer MUST inspect ZIP and BagIt
structure; it MUST NOT trust an extension or declared media type.

The ZIP bytes need not be deterministic. The canonical Bag contents and
manifests establish snapshot content and fixity.

A safe archive profile MUST:

- allow only regular files and directories;
- reject symlinks, hardlinks, devices, sockets, FIFOs, and encrypted entries;
- reject absolute paths, drive or UNC paths, NUL, backslash ambiguity, `..`,
  duplicate names, and file/directory conflicts;
- detect case-folding and Unicode-normalization collisions;
- impose local limits on entries, depth, path length, compressed bytes,
  expanded bytes, compression ratio, CPU, memory, and wall time;
- ignore archived ownership, permissions, ACLs, extended attributes, and
  timestamps;
- never recurse into nested archives automatically;
- stage into a new owner-only temporary directory using descriptor-relative,
  no-follow operations; and
- verify BagIt manifests and Chutni semantic constraints before committing any
  imported content.

Importers MUST use exact, locally bundled versions or pinned digests of every
supported JSON-LD context and validation schema. They MUST reject an unknown
required context or profile and MUST NOT use a mutable cached remote context.
Every imported IRI is opaque and non-dereferenceable by default.

JSON parsing MUST require UTF-8, reject duplicate object keys, bound nesting,
numbers, strings, collections, and JSON-LD expansion, and reject values that
cannot be represented consistently by the profile. Importers MUST NOT fetch
remote contexts or payload URLs merely because an imported graph references
them.

Import is atomic. An importer MUST stage objects without overwriting existing
paths, validate the entire required closure, resolve ID/content conflicts, and
publish records and bytes in one transaction or equivalent atomic commit. A
failure exposes no partial records to queries and leaves only bounded,
garbage-collectable staging data.

## 14. Local binding and live-store behavior

Portable identity and local authority are separate.

A local `Binding` has a recipient-local opaque identifier and associates one
Source with one authorized resolution route. A Source may have multiple
Bindings, each with a different observed revision or branch. A Binding may
record:

- an authorized root;
- a relative logical path;
- an absolute native path;
- OS file identity;
- the last observed revision;
- verification time; and
- local sensitivity, retention, and disclosure policy.

This state MUST live in a protected recipient-local store and is excluded from
an ordinary pack. A private same-machine export profile may carry additional
locators, but it MUST be explicit, visibly previewed, and never the default.

After import:

1. every Source begins unbound unless the recipient already has a trusted
   binding;
2. an imported locator grants no permission;
3. the user or an authorized local policy maps a logical source or root to a
   candidate;
4. the application opens the candidate through the authorized root without
   following an unapproved link or mount transition;
5. it hashes the exact stable handle used for reading against a known
   SourceRevision; and
6. it uses that same handle for the authorized operation or reopens and
   reverifies immediately before use.

Only then may it describe that binding as matching the revision. A final
component swap, symlink, junction, reparse point, alias, or mount escape requires
a new containment check and, when outside the root, new authorization.

HTTP, object-store, network-share, device-path, or other non-local locators
require separate authorization. Importers MUST NOT automatically dereference
them.

A reference live implementation may use SQLite with one coordinating writer
and snapshot readers. Another may use a service and Postgres. Chutni
interoperability depends on import and export behavior, not their internal
schema. A v0.1 implementation MUST serialize writers or use transactional
conflict detection; Chutni does not define cross-application multi-writer merge
semantics.

Applications may maintain FTS, vector, graph, or hybrid indexes. These are
derived local projections. They may be discarded and rebuilt without loss of
Chutni Core information.

## 15. Conformance modules and candidate profiles

Modules are independently declared and tested. Core, Exchange, Bag, and Pack
responsibilities are separate; there is no vague “full implementation” badge.

### 15.1 Core Graph Reader

A Core Reader:

- understands the core entity and relation model;
- enforces Core identity, immutability, target, provenance, and selector
  invariants;
- verifies descriptors when payload bytes are supplied;
- exposes unknown optional terms as unsupported rather than guessing;
- does not auto-bind or auto-open locators; and
- treats every imported value as untrusted.

### 15.2 Core Graph Writer

A Core Writer:

- emits a closed graph for the selected export;
- anchors every content Artifact to exact revisions;
- records every producer-controlled or known material derivation input and
  declares unavailable dependencies;
- generates privacy-safe metadata;
- emits correct payload descriptors; and
- validates its own output before release.

### 15.3 Exchange Reader and Writer

An Exchange Reader or Writer maps the Core graph to the exact Chutni RO-Crate
JSON-LD profile. It uses only pinned contexts, follows the required provenance
closure in Section 13.1, and passes the fixed syntax, SHACL, and procedural
validation suites. It does not read or write BagIt manifests.

### 15.4 Bag and Pack Reader and Writer

A Bag Reader or Writer validates or creates the directory rules in Section
13.2. A Pack Reader or Writer additionally validates or creates the ZIP64
serialization and archive-safety rules in Section 13.3. A product may support
the Exchange graph without supporting archive files.

### 15.5 Local File Binding

This profile standardizes safe logical relative paths and recipient-controlled
root mapping. Absolute native paths and OS identity remain local by default.
Descriptor-relative, no-follow traversal and stable-handle verification are
mandatory. Symlink, junction, reparse-point, alias, and mount traversal MUST NOT
leave an authorized root without new authorization.

### 15.6 Candidate Acceleration Profile

Acceleration is outside v0.1 conformance. A future embedding profile would need
to record at least:

- the exact input Artifact ID and payload digest;
- embedding model provider, identifier, and revision or weights digest;
- tokenizer and preprocessing revision where applicable;
- dimensions and data type;
- pooling and normalization;
- vector count and ordering;
- a fully specified declarative binary encoding, byte order, shape, and decoded
  size formula;
- numeric validity rules, including NaN and infinity handling; and
- payload byte length and digest.

If any required compatibility field is unknown or unsupported, the consumer
MUST discard or ignore the accelerator. It may still use the underlying text or
other canonical Artifact.

Application search indexes, approximate-nearest-neighbor graph files, token
caches, and model runtime state are excluded from this profile until a safe,
portable, independently justified encoding exists.

### 15.7 Candidate Gateway and MCP Profile

A gateway may expose read operations as MCP Resources and search or mutation
operations as typed MCP tools. The binding should offer concepts such as:

- enumerate Sources and Revisions;
- search local projections;
- fetch Artifact metadata;
- read bounded Artifact payload ranges;
- resolve selectors;
- add a new Artifact and provenance Activity; and
- request a separately authorized source read.

Gateway authentication, capability scope, rate limits, disclosure controls, and
audit behavior are mandatory for the binding. “Listening only on localhost” is
not authentication.

The model never receives ambient filesystem access. Search results, paths,
hashes, snippets, metadata, embeddings, and existence information all count as
potential disclosure.

### 15.8 Future bridges and protection profiles

A future producer may project selected Chutni Artifacts into the young Open
Knowledge Format project for human- and agent-readable curated knowledge. Any
such view is not canonical evidence and should link back to exact Chutni
Artifacts and SourceRevisions. Changes to the projection do not mutate the
Chutni graph.

Long-term repositories may later map Chutni Packs into OCFL. Media with existing
C2PA Content Credentials should retain them. Future signature, attestation, and
recipient-encryption profiles must reuse reviewed standards and clearly
separate:

- byte integrity;
- exporter authentication;
- producer attestation;
- semantic correctness; and
- confidentiality.

A signature by an exporter authenticates the exporter and signed bytes. It does
not prove that a named model produced the Artifact, that a human reviewed it, or
that a summary is true.

## 16. How applications use Chutni

### 16.1 Producer workflow

1. Assign or recover a logical Source identity.
2. Read a stable snapshot of the source.
3. Calculate its size, media type, and SHA-256 digest to create R1.
4. Run extraction or inference as an Activity using R1 and any other exact
   inputs.
5. Store each output as an immutable Artifact and Blob.
6. Record the software Agent, model or parser Entity, Plan, parameters, and
   input roles.
7. Anchor selections to exact targets using standard selectors.
8. Export only the user-selected graph closure and payloads.
9. Redact local bindings and secrets.
10. Generate and validate the Chutni Bag, then serialize a Chutni Pack when an
    archive is needed.

### 16.2 Consumer workflow

1. Treat the Chutni Pack or Bag as hostile input.
2. Safely unpack within configured resource limits.
3. Verify BagIt completeness and every payload digest.
4. Validate the graph, exact profile version, entity invariants, structurally
   check selectors, resolve selector bounds only for available targets, and
   validate the required provenance closure.
5. Atomically import allowlisted records and bytes through protected staging
   into a fresh or existing protected local store.
6. Mark origins and trust state locally; do not accept self-asserted trust.
7. Optionally ask the user to bind Sources to authorized local candidates.
8. Verify candidate bytes before associating them with a known revision.
9. Build local search indexes.
10. Reuse only Artifacts whose types and compatibility requirements are
    understood.

### 16.3 Consumer answering a question

1. Search its local index for candidate Artifacts.
2. Filter or rank by exact revision lineage, provenance, trust, and local
   policy.
3. Retrieve bounded content and its selectors.
4. Present retrieved content to the model as quoted, untrusted context.
5. Reopen the original source when precision, policy, or risk requires it.
6. Cite the exact SourceRevision and selector, and disclose when the original
   source was unavailable.

### 16.4 Adding improved work

A second application does not “upgrade” an existing Artifact in place. It
creates:

- a new Activity;
- a new Artifact;
- explicit derivation from the same inputs or an earlier Artifact; and
- an optional relation explaining that it corrects, revises, or is preferred
  for a particular purpose.

Consumers decide which Artifact to use. Chutni preserves the alternatives and
their evidence.

## 17. Worked examples

The examples in this section use simplified logical JSON. They illustrate the
model and are not the final RO-Crate wire syntax.

### 17.1 First extraction

An application observes `report.pdf` as R1 and extracts text:

```json
{
  "source": {
    "id": "urn:uuid:0195f000-0000-7000-8000-000000000001",
    "name": "Quarterly report",
    "kind": "document"
  },
  "revision": {
    "id": "urn:uuid:0195f000-0000-7000-8000-000000000002",
    "specializationOf": "urn:uuid:0195f000-0000-7000-8000-000000000001",
    "payload": {
      "mediaType": "application/pdf",
      "digest": "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "size": 492801
    },
    "included": false
  },
  "activity": {
    "id": "urn:uuid:0195f000-0000-7000-8000-000000000003",
    "type": "text-extraction",
    "used": ["urn:uuid:0195f000-0000-7000-8000-000000000002"],
    "agent": "urn:example:software:pdf-extractor:4.1"
  },
  "artifact": {
    "id": "urn:uuid:0195f000-0000-7000-8000-000000000004",
    "kind": "extracted-text",
    "target": "urn:uuid:0195f000-0000-7000-8000-000000000002",
    "generatedBy": "urn:uuid:0195f000-0000-7000-8000-000000000003",
    "payload": {
      "mediaType": "text/plain; charset=utf-8",
      "digest": "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
      "size": 18432
    }
  }
}
```

The PDF bytes need not be inside the pack. The Artifact is still exactly bound
to the digest of R1.

### 17.2 The source changes from R1 to R2

The application later observes different bytes:

```json
{
  "id": "urn:uuid:0195f000-0000-7000-8000-000000000005",
  "specializationOf": "urn:uuid:0195f000-0000-7000-8000-000000000001",
  "wasRevisionOf": "urn:uuid:0195f000-0000-7000-8000-000000000002",
  "payload": {
    "mediaType": "application/pdf",
    "digest": "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
    "size": 501244
  }
}
```

It creates new extraction Activity E2 and Artifact A2 for R2. It does not mark
the R1 Artifact stale, rewrite its source hash, or delete it. Local retrieval
may prefer R2 for a binding through which R2 was most recently verified.

### 17.3 Import and local rebinding

Application B imports a pack from Application A:

1. B validates and imports the pack; every Source is initially unbound.
2. The user maps “Quarterly report” to a file inside an authorized folder.
3. B hashes the candidate and finds that it matches R2.
4. B can now open that file under its own permission policy and prefer
   R2-derived Artifacts.
5. If the digest matches neither R1 nor R2, B records a new observation or
   leaves the Source unmatched. It never pretends that a pathname proves
   identity.

### 17.4 A stronger model adds a summary

Application B understands the imported extracted-text Artifact but not
Application A's embedding. It:

- reuses the text without rerunning PDF extraction;
- discards the unsupported embedding;
- runs its own summarization Activity against the exact text Artifact;
- records the model revision, recipe, and parameters; and
- exports the new summary plus the transitive provenance needed to interpret
  it.

This is successful interoperability even though the embedding was not portable.

### 17.5 Human correction

A human notices that OCR Artifact A1 says “$I0,000” instead of “$10,000.” The
editor creates corrected Artifact H1:

- `H1 prov:wasDerivedFrom A1`;
- a human correction Activity generated H1;
- H1 uses a selector targeting the exact erroneous range in A1;
- H1 uses Web Annotation's `editing` motivation, or
  `prov:wasRevisionOf A1` when H1 is a complete revised representation; and
- A1 remains unchanged for audit and comparison.

The human identity is declared provenance. It becomes authenticated only if an
external identity and signature mechanism establishes that claim.

### 17.6 Multi-document synthesis

A model writes a comparison using Report R2, Spreadsheet R7, and Transcript R3.
The synthesis Artifact records all three exact input revisions, any intermediate
text or table Artifacts, their roles, and the generating Activity. It is not
assigned to one arbitrary `source_id`.

Each claim may use a Web Annotation selector against the exact supporting
Artifact. A consumer can reuse the synthesis while still tracing individual
claims back to exact representations.

### 17.7 Cloud model through a local gateway

A user permits a local gateway to send three selected excerpts to a cloud
model. The gateway:

- authenticates the requesting client;
- checks a capability scoped to the destination, purpose, Sources, content
  types, byte limit, and duration;
- resolves each selector locally;
- removes absolute paths and unnecessary identifiers;
- sends only the approved bytes; and
- records a local audit event.

Neither the imported pack nor instructions inside a retrieved document can
grant or widen this permission.

### 17.8 Hypothetical reference CLI

A future reference CLI might look like this:

```sh
chutni ingest ./report.pdf --into local-store
chutni export --source urn:uuid:... --output report.chutnipack
chutni validate report.chutnipack
chutni import report.chutnipack --into another-local-store
chutni bind urn:uuid:... ./report.pdf
chutni inspect --lineage urn:uuid:...
```

These commands are illustrative. The CLI is not the protocol, and conforming
applications need not implement these names.

## 18. Security, privacy, and trust

The controls in this section are requirements for any Chutni module or future
binding to which they apply. A profile MUST NOT claim conformance while treating
them as optional deployment advice.

### 18.1 Trust boundary

Every transfer pack is untrusted input, including packs that pass integrity
checks. Integrity proves that bytes match the manifest; it does not make them
safe or true.

An importer MUST validate and copy accepted data into newly created or existing
protected local storage through allowlisted operations. It MUST NOT use a
foreign catalog or database in place.

Imported IDs, relations, producer names, timestamps, policies, locators,
tombstones, MIME types, selectors, and extensions are attacker-controlled
claims. An ID collision MUST NOT overwrite local data. If the same identifier
arrives with conflicting immutable content, the importer MUST reject or
quarantine it. A local namespacing projection is allowed only when it preserves
the original graph and identifiers, rewrites every affected edge atomically,
is excluded from signatures, and is clearly exposed as an import projection;
silent remapping is forbidden.

### 18.2 Prompt injection, parsing, and rendering

All string-bearing fields are untrusted, including:

- extracted text and OCR;
- captions, summaries, annotations, and human notes;
- filenames, titles, paths, and URLs;
- producer, model, and recipe names;
- errors and logs;
- relation labels and extension fields; and
- HTML, Markdown, terminal control characters, and bidirectional text.

Retrieved content MUST NOT grant permissions, change system or developer
policy, authorize tools, request additional source reads, approve disclosure,
execute code, or delete data. Typed host logic and separately authorized user
intent MUST control those actions. Prompt delimiters are useful presentation,
not a security boundary.

UIs MUST render imported HTML, Markdown, URLs, terminal escapes, and scripts
inert by default.

Media types are attacker claims, not permission to invoke a handler. Risky PDF,
Office, image, audio, video, SVG, HTML, Markdown, archive, and selector payloads
MUST be parsed with least privilege, finite decode limits, and isolation
appropriate to the platform. Import MUST NOT run macros or scripts, resolve
external entities, load remote subresources, or automatically open payloads in
an OS application. `SvgSelector` values MUST be reduced to an inert geometry
subset or sanitized by a parser that rejects scripts, animation, external
references, styling, text, and active content.

### 18.3 Filesystem safety

The Local File Binding Profile MUST use descriptor-relative, no-follow
traversal. Symlinks, junctions, reparse points, aliases, and mount transitions
MUST NOT escape an approved root. Devices, sockets, pipes, and other
non-regular files require a specialized, explicitly authorized producer.

Pack staging, live stores, locks, databases, objects, local policy, and scratch
space MUST use owner-only access where the platform supports it: approximately
`0700` directories and `0600` files on POSIX, with equivalent controls
elsewhere. An implementation MUST warn or refuse its secure profile when the
target filesystem cannot enforce the required isolation. Producers MUST avoid
indexing the Chutni store, its backups, and its scratch space.

### 18.4 Resource exhaustion

Every importer, parser, scanner, search service, and gateway MUST start with
documented, finite, safe defaults and enforce them while streaming, parsing,
expanding JSON-LD, and allocating memory. Attacker-declared sizes MUST NOT
trigger allocation before independent bounds checks. An explicit local action
may raise limits. Limits cover:

- files, graph entities, relations, and graph depth;
- source, pack, payload, inline value, and aggregate bytes;
- archive expansion and compression ratios;
- JSON depth and string length;
- image pixels, spreadsheet cells, media duration, and archive nesting;
- embedding dimensions and aggregate vector bytes;
- query cost, result count, snippets, and response bytes; and
- CPU, memory, wall time, request rate, and cancellation.

Implementations MUST handle sparse files, continually mutating files, malformed
media, integer overflow, NaN or infinity in numeric artifacts, prohibited
derivation/revision cycles, pathological search queries, and parser crashes
without committing partial Artifacts.

### 18.5 Privacy and disclosure

A derived store may be more sensitive than the sources because it centralizes
hidden text, OCR, filenames, entities, summaries, activity times, and
searchable representations.

Ordinary export is redacted and selective. An exporter SHOULD preview exactly
which Sources, Artifacts, identifiers, and original bytes will leave the local
store. It SHOULD omit unnecessary timestamps, host/user/device correlation
identifiers, private paths, incidental request or hardware fingerprints, and
secret-bearing prompt text. It MUST preserve provenance-critical model
identifiers/revisions and the stable Chutni entity IDs required by the exported
lineage.

Stable entity IDs permit cross-pack merging and also permit correlation. A
privacy-maximizing export MAY instead create an explicitly labeled,
pack-scoped pseudonymous projection. It MUST rewrite every internal edge
consistently, preserve the original graph only locally, disclose that
cross-pack continuity was intentionally removed, and never pretend that the new
IDs identify the original entities globally.

Content hashes can reveal membership of known content. Implementations SHOULD
avoid cross-user or global deduplication and treat hashes as sensitive
metadata.

Putting a live store or pack in a cloud-synced folder, backup, shared home
directory, external search index, or remote logging system is a disclosure and
MUST be treated accordingly.

### 18.6 Local trust state

Consumers may maintain local acquisition or trust labels such as:

- locally generated and source-matched;
- imported with authenticated exporter;
- imported without authenticated exporter;
- source bytes included and digest-verified;
- source-referenced but locally unavailable; and
- human-reviewed under a named local policy.

These labels are local conclusions. A pack cannot self-assert the trust label
that a receiver must give it.

No standardized model self-confidence field belongs in Core. Confidence is
often incomparable across models and tasks. Chutni records objective evidence
and lets consumers make their own quality judgment.

### 18.7 Retention and deletion

Immutability is a modeling rule, not a command to retain secrets forever. A
local retention policy may remove payloads while retaining minimal provenance,
or remove an entire lineage.

A local operation must distinguish unbinding, hiding, forgetting a revision,
and purging a lineage. When an authorized purge selects a SourceRevision or
Artifact, retrieval denial for that scope and all transitive descendants MUST
be immediate, including multi-input syntheses, embeddings, snippets, indexes,
gateway-result caches, and other derived access paths. Garbage collection then
removes unshared Artifacts, Blobs, indexes, caches, journals, and temporary
files according to local policy. A physically shared Blob may remain for an
unaffected lineage, but the purged lineage MUST NOT remain an authorization
path to it. Secure erasure cannot be promised on every filesystem or backup
medium.

An imported tombstone or supersession relation MUST NOT delete or hide local
data. Synchronization and authenticated deletion propagation are outside v0.1.

### 18.8 Gateway security

A conforming gateway binding requires:

- read-only operation by default;
- authenticated client identity;
- separate search, metadata, content-read, write, disclosure, and deletion
  capabilities;
- Source, root, Artifact, destination, byte, and time scopes;
- OS-protected local IPC or TLS with strong client authentication remotely;
- origin, CSRF, and DNS-rebinding defenses when HTTP is used;
- rate limiting, pagination, cancellation, and bounded reads; and
- protected audit records.

Write, external disclosure, and deletion require separately authorized current
user intent, narrow capability scopes, and mutation preconditions. A retrieved
prompt, an imported capability-like value, or possession of a broad client
credential is insufficient. Imported rights, sensitivity, retention, or policy
declarations may tighten or inform local policy but MUST NOT grant or relax
access.

Per-application policy is enforceable only through OS controls, encryption, or
exclusive gateway mediation. If every application can directly read the
underlying files, a gateway cannot enforce those distinctions.

## 19. Conformance and validation

Conformance is modular:

- Chutni Core Graph Reader or Writer;
- Chutni Exchange Reader or Writer;
- Chutni Bag Reader or Writer;
- Chutni Pack Archive Reader or Writer;
- Local File Binding; and
- separately specified future Gateway, acceleration, archival, attested,
  signed, or confidential profiles.

An implementation claims only the modules and exact profile versions it passes.
During the 0.x series, a different minor version is not presumed compatible.
An implementation MUST NOT silently treat an unsupported optional profile as
understood.

The reference conformance suite should include:

- a minimal detached-source pack;
- a pack with included source bytes;
- the R1/R2 case;
- multi-input synthesis;
- human correction;
- a shared Blob with distinct Artifact provenance;
- root rebinding;
- a partial export with complete transitive closure;
- unknown optional terms and an unknown required profile;
- missing, truncated, mismatched, and extra payloads;
- structurally invalid selectors, unresolved detached selectors, resolved
  bounds failures, and coordinate-space errors;
- invalid provenance and prohibited revision/derivation cycles;
- duplicate IDs with conflicting content;
- path traversal, symlink, hardlink, Unicode collision, and archive-bomb cases;
- malicious remote JSON-LD contexts and payload URLs; and
- resource-limit and cancellation cases.

Parsers, archives, JSON-LD, selectors, and media payloads SHOULD be fuzzed.
Validation SHOULD report syntax, integrity, semantic, profile, and local-policy
failures separately.

Prompt injection in every string-bearing field, gateway authorization, inert
rendering, parser isolation, disclosure, and deletion belong in runtime binding
tests. They are security conformance failures, not merely static graph
validation errors.

## 20. Compatibility and artifact reuse

Compatibility is decided per Artifact, not per pack.

| Artifact | Safe default |
|---|---|
| UTF-8 extracted text or OCR | Reuse when digest, media type, language, and selector semantics validate. |
| Caption, summary, or annotation | Reuse as advisory content with provenance visible. |
| Table | Reuse when schema, dialect, encoding, and units are understood. |
| Thumbnail or page image | Reuse when media decoding is sandboxed and selectors target it exactly. |
| Embedding | Ignore in v0.1; reuse only if a future exact Acceleration Profile is supported. |
| Search index | Rebuild locally. |
| Token IDs or KV cache | Ignore unless a future narrow profile proves exact compatibility and safety. |
| Unknown binary or serialization | Do not execute or deserialize; preserve only if local policy permits. |

Chutni does not guarantee bit-for-bit reproducibility of model outputs.
Provenance helps consumers compare and judge work; it does not make a
nondeterministic or remotely hosted model reproducible.

## 21. Versioning, extensions, and governance

Before 1.0, compatibility may change as implementation evidence is gathered.
Every ExchangeSnapshot identifies exact profile versions. During 0.x, a consumer MUST
support the exact declared version; it does not infer minor-version
compatibility. A post-1.0 profile must publish its compatibility rules.

Core terms use a stable Chutni namespace. Extension terms use stable,
owner-controlled IRIs. An extension:

- MUST document its semantics and security considerations;
- MUST NOT weaken Core requirements;
- MUST NOT redefine a Core term;
- MUST declare whether it is required to interpret a particular Artifact; and
- MUST be safely ignorable when it is optional.

Unknown metadata need not be blindly round-tripped. A sanitizing exporter may
drop unknown or sensitive extensions, and an importer may quarantine or discard
them under explicit quotas. If a required profile is unsupported or removed,
the exporter MUST also remove every dependent Artifact and MUST NOT claim that
profile. This prevents compliant applications from becoming carriers for
opaque hostile payloads or emitting artifacts they can no longer interpret.

Chutni metadata does not grant copyright, database, privacy, or redistribution
rights. Exporters and consumers MUST preserve and obey applicable rights and
license metadata for source bytes and derivative payloads.

Recommended project governance:

- a neutral standalone repository and namespace;
- specification text under CC BY 4.0;
- schemas, validators, fixtures, and reference code under Apache-2.0 or MIT;
- a public proposal and decision record process;
- no normative dependency on a single vendor or implementation;
- at least two maintainers from different implementations before 1.0; and
- published interoperability and security test results.

Samosa should be described only as the first or an example implementation.
Normative examples should use neutral producers and models.

## 22. Existing work and overlap

### 22.1 Generic AI memory

The “portable AI memory” category is already crowded. Most entries below are
young, self-published drafts rather than adopted formal standards:

- [Portable AI Memory](https://portable-ai-memory.org/spec/v1.0/) covers
  preferences, goals, identity, conversations, confidence, lifecycle, and
  related user memory.
- [Open Memory Protocol](https://openmemoryprotocol.com/spec/) targets memory
  records and conversation migration.
- [EngramSpec](https://engramspec.org/) targets portable, governed user and
  agent memory.
- [Agent Memory Protocol](https://agentmemoryprotocol.io/) uses a
  Markdown-oriented memory model.
- [Universal Memory Protocol](https://universalmemoryprotocol.io/) targets
  transport-neutral operations over agent memory records.
- The [W3C AI Agent Memory Interoperability Community
  Group](https://www.w3.org/groups/cg/ai-agent-memory-interop/) is working in
  the same broad area; Community Group work is not a W3C Recommendation.
- [AIMEM Bundle](https://www.ietf.org/archive/id/draft-vu-aimem-bundle-00.html)
  and [SAIHM](https://datatracker.ietf.org/doc/draft-saihm-memory-protocol/) are
  individual Internet-Drafts. Internet-Drafts are works in progress, not IETF
  standards or endorsements.

These projects are recent and do not yet constitute one universally adopted
standard, but Chutni should not enter that race. If Chutni expands into
personality, preferences, goals, conversation history, identity, or memory
decay, it should stop and adopt or bridge to the relevant effort rather than
invent another format.

### 22.2 Open Knowledge Format

OKF v0.2 is a notable close functional overlap, but it is also a young
Google-hosted project rather than an adopted formal standard. It uses Markdown
with YAML front matter to exchange curated knowledge for people and agents,
with provenance, trust, lifecycle, and attestation signals.

Use OKF instead of Chutni when the desired product is:

- an agent-readable knowledge repository;
- curated concepts, playbooks, metrics, or documentation;
- easy authoring and Git diffs; or
- human-readable Markdown as the primary artifact.

Use Chutni when the desired product requires:

- exact source byte revisions;
- machine-verifiable derivative payloads;
- representation-specific selectors;
- detailed multi-step processing lineage;
- reuse of OCR, extraction, transcription, or other preprocessing; or
- privacy-safe rebinding to local sources.

The formats are complementary. Chutni should define an optional OKF projection
instead of recreating OKF.

### 22.3 RO-Crate, PROV, BagIt, and Web Annotation

These standards cover much of Chutni's mechanics:

- RO-Crate is the general package metadata graph and profile mechanism.
- PROV is the provenance model.
- BagIt is the fixity and transfer inventory.
- Web Annotation is the targeting and selector model.

RO-Crate plus Process Run Crate already gets surprisingly close to the graph
and run-provenance portion of Chutni. The remaining justification for Chutni is
the exact logical Source-to-SourceRevision contract, the AI Artifact vocabulary
and compatibility rules, precise safe-retrieval defaults, recipient-local
binding separation, and the exclusion of indexes and runtime caches. Every new
Chutni term should be justified against those gaps.

Chutni is not a replacement for any of them. It is a domain profile that says
which pieces are required and how they work together for source-derived AI
artifacts.

### 22.4 RAG frameworks, vector stores, and MCP

RAG frameworks and vector databases solve indexing and retrieval inside an
application. They usually do not provide a neutral, immutable exchange contract
for exact source revisions, derivative payloads, selectors, and processing
lineage.

MCP gives models and applications a runtime way to discover resources and call
tools. It does not define the durable meaning or packaging of Chutni Artifacts.
A Chutni MCP binding is useful, but MCP does not make Chutni redundant.

### 22.5 The honest novelty claim

Chutni must not claim to have invented:

- portable AI memory;
- content-addressed storage;
- immutable versions;
- provenance graphs;
- annotations or selectors;
- data packaging;
- RAG or vector search;
- model gateways; or
- cryptographic integrity.

Its plausible novelty is a practical profile for **user-controlled, exportable,
cross-application reuse of AI preprocessing and interpretation artifacts tied
to exact source revisions**, with exact anchors, detailed declared provenance,
recipient-local rebinding, and a clean split between durable derivatives and
disposable acceleration.

That claim is credible but unproven. Independent interoperability, not the
specification's length, will prove it.

## 23. Recommended v0.1 scope

The smallest credible release should include only:

1. Source, SourceRevision, Blob, Artifact, Target, Activity, Agent, Plan, and
   ExchangeSnapshot.
2. Immutable R1/R2 semantics and multi-input provenance.
3. SHA-256 OCI-shaped payload descriptors.
4. UTF-8 extracted text, OCR, transcript, caption, summary, and human correction
   Artifact kinds.
5. Web Annotation text and byte selectors, plus well-defined media fragments.
6. RO-Crate exchange metadata.
7. Chutni Bags using BagIt and one hardened ZIP64 Chutni Pack serialization.
8. Redacted export and recipient-local file rebinding.
9. Core Graph, Exchange, Bag, and Pack Reader/Writer validation.
10. Golden, malicious, and fuzz fixtures.
11. A validator and two small independently written adapters.

Defer from v0.1:

- conversational and personal memory;
- synchronization and multi-writer mutation;
- ranking algorithms;
- vector databases and shared live indexes;
- token and KV caches;
- remote source fetching;
- arbitrary executable transformations;
- universal embedding portability;
- a new HTTP API where MCP or a local library suffices;
- custom signatures or encryption; and
- whole-drive graph optimization until measured.

A single RO-Crate JSON-LD graph may become unwieldy at hundreds of thousands or
millions of entities. Benchmark large stores before freezing a whole-store
serialization. Prefer selective transfer packs. Add streaming or sharded
exchange only when measurements demonstrate the need.

## 24. Delivery roadmap

### Phase 0: Scope and proof

- Publish this foundation document in a neutral repository.
- Validate the use case with two application teams.
- Write one end-to-end exchange story before expanding the vocabulary.
- Decide on a stable namespace and project license.

### Phase 1: Implementable profile

- Publish the RO-Crate profile and term mapping.
- Publish the fixed JSON syntax schema, SHACL graph shapes, and procedural
  semantic checks.
- Publish the deterministic Process Run Crate crosswalk.
- Specify the BagIt and ZIP rules precisely.
- Build the validator first.
- Add minimal, R1/R2, multi-input, and malicious fixtures.
- Build one simple producer and one separately implemented consumer.

### Phase 2: Demonstrate reuse

- Exchange text extraction and OCR without reprocessing.
- Preserve exact selectors and multi-step provenance.
- Demonstrate local rebinding without path leakage.
- Publish compatibility failures as well as successes.
- Revise the profile based on measured implementation friction.

### Phase 3: Optional bindings

- Add a narrowly scoped MCP binding.
- Add an OKF projection.
- Evaluate the embedding Acceleration Profile.
- Evaluate C2PA, attestation, confidential transfer, and OCFL archival profiles.

Do not declare 1.0 until two independent implementations pass the same suite
and exchange real artifacts in both directions.

## 25. Open design questions

These questions should be answered through prototypes and tests:

1. Which stable Chutni namespace and profile URI will the project control?
2. Which minimal Artifact kinds are required for v0.1?
3. Which existing selector vocabulary best covers PDF pages, spreadsheet
   sheets/cells, slides, and archive members?
4. Should UUIDv7 URNs be the recommended entity IDs, or should more entities use
   content-derived identifiers?
5. Does the required closure in Section 13.1 preserve enough provenance without
   making selective packs unnecessarily large?
6. Which RO-Crate Process Run patterns can be reused unchanged?
7. What JSON-LD validation can be expressed in SHACL, and which invariants need
   procedural validation?
8. Does the pack need deterministic metadata bytes, or are BagIt fixity and a
   canonical graph sufficient?
9. Which ZIP limits are fixed by the profile and which remain recipient policy?
10. What is the minimum useful local file-binding exchange without leaking
    host identity?
11. Are inline text payloads worth the added canonicalization complexity?
12. Can an embedding profile demonstrate reuse across two real implementations,
    or should it remain out of v0.1?
13. How large can one RO-Crate graph become before selective or sharded exchange
    is necessary?
14. Which signing and confidential-transfer standards fit the pack threat model
    without creating a bespoke cryptosystem?

## 26. Success criteria

Chutni succeeds when:

- a user controls, can export, and can inspect the artifacts they are permitted
  to use;
- Application B reuses work created by Application A;
- both applications agree on exact source revisions and selector coordinates;
- provenance survives the transfer without being confused for proof;
- local paths and permissions do not leak or transfer as authority;
- unsupported accelerators fail safely while canonical artifacts remain useful;
- indexes can be rebuilt independently;
- malicious packs are rejected predictably;
- Samosa can be removed from every normative example without changing the
  protocol; and
- implementers choose Chutni because it is smaller and safer than inventing
  another private format.

That is a realistic target. A universal AI memory layer is not.

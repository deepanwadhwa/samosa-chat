# Chutni protocol integration

The Chutni protocol is specified and governed in the independent
[chutni-protocol repository](https://github.com/deepanwadhwa/chutni-protocol).
Samosa pins that repository at `vendor/chutni` and ships its generic
`chutni-mcp` service as part of the normal application runtime.

Samosa is the first guinea-pig host; it has no private extension to the store
format.

## End-user flow

1. Install Samosa. Chutni does not need to be installed separately.
2. Open **Chutni** in Samosa and choose a folder `P`.
3. Samosa calls `chutni_folder_status` and shows the exact adjacent store path
   `P.chutni`, whether it will be created or reused, and the scan policy.
4. Click **Build memory**.
5. Samosa calls `chutni_folder_activate` with the user's confirmation. The
   bundled service creates or opens `P.chutni`, authorizes `P`, scans it, and
   records Samosa in producer provenance.
6. On the ready folder, click **Use in this chat**.
7. Before each turn in that chat, the gateway calls `chutni_search`, bounds the
   results, labels every excerpt with its source path, marks the block as
   untrusted file data, and only then sends it to Samosa's local model.

Another Chutni-capable application can open the same `P.chutni` store without
migration. Conversely, Samosa opens and refreshes a conforming adjacent store
that another host already created.

## Architecture

```text
Samosa UI / local model
        │
        │ one-shot JSON tool calls
        ▼
bundled chutni-mcp service
        │
        ▼
libchutni ── P.chutni
```

MCP-capable hosts launch the same executable over stdio. Samosa uses
`chutni-mcp --call TOOL JSON` because its C gateway already supervises local
child processes. These are two transports over the same tool handlers, not
different integrations or store implementations.

Samosa keeps small presentation-only documents under
`~/.samosa/chutni/`—scope name, job state, and events. They are not a memory
store and never appear in retrieval. The portable store beside the selected
folder is the sole source of indexed evidence.

The older `src/samosa_chutni_db.c` implementation remains temporarily for its
standalone legacy tests and migration research. The gateway no longer invokes
it. It must not be packaged or described as a Chutni store.

## Current capability

- Adjacent-store create/open/reuse
- Explicit confirmation before scans
- BLAKE3 source identity and freshness
- Text-like whole-file extraction
- Metadata artifacts for other regular files
- Lexical retrieval with paths, freshness, score type, and provenance IDs
- Cross-host create/update/read handoff
- Portable-store preservation when a user removes a folder from Samosa

PDF page text, OCR, image captions, spreadsheets, audio, hybrid retrieval, and
root remapping are not implemented in the reference scanner yet. Samosa must
not imply those legacy sidecar capabilities are present on the protocol path.

## Development

Clone with the pinned dependency:

```sh
git clone --recurse-submodules https://github.com/deepanwadhwa/samosa-chat.git
```

For an existing checkout:

```sh
git submodule update --init
make chutni-gateway-test
```

`make samosa-gateway` builds `build/chutni-mcp` from the pinned submodule.
`tools/install_local_dev.sh` and the release installer place it at
`current/bin/chutni-mcp`; the end user does nothing additional.

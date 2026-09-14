# Qwen36B bounded string-copy hardening

Status: proposed hardening; no known active vulnerability

Date: 2026-08-31

Scope: `src/qwen36b.c`
Related review: GitHub PR #13

## Decision

Remove the remaining `strcpy` calls from `src/qwen36b.c`, but do not replace
them with unchecked `snprintf(dst, size, "%s", src)` calls. Use exact,
fail-closed copies instead: reject any source string that cannot fit in the
destination, and copy it only after the length has been validated.

This is defensive hardening, not an emergency vulnerability response. The
current call sites are not exposed to arbitrary HTTP or LAN strings, and four
of the five copies already have bounds established by their surrounding code.
The cleanup is still worthwhile because it makes those invariants explicit,
prevents a future caller from invalidating them, and removes a class of static
analysis findings.

## Current risk assessment

The scanner finding is pattern-based: it reports every `strcpy` call without
proving whether the source can exceed the destination.

The five current copies have the following properties:

| Location | Source | Current protection | Assessment |
| --- | --- | --- | --- |
| `qt_load`: `name` to `nm[512]` | Internal tensor name | All current callers use short literals or names formatted into another 512-byte buffer | Not presently attacker-controlled, but the destination bound is not enforced locally |
| `qt_load`: `wrapped` to `nm[512]` | A 512-byte local buffer | `wrapped` is produced by bounded `snprintf` | Copy is bounded, although truncation of the formatted name is not checked |
| `teacher_fsync_parent`: `path` to `parent[2048]` | Teacher CLI output path | Rejects `strlen(path) >= sizeof(parent)` immediately before copying | Bounded |
| `teacher_fsync_parent`: `"."` to `parent[2048]` | Two-byte constant including NUL | Constant necessarily fits | Bounded |
| `serve_mkdir`: `path` to `copy[PATH_MAX]` | Configured or constructed storage path | Rejects `strlen(path) >= sizeof(copy)` immediately before copying | Bounded |

Consequently, the `HIGH` scanner severity should not be interpreted as proof
of a remotely exploitable buffer overflow. In particular, the copies are not
fed directly by chat messages, document contents, or an unrestricted
conversation ID. The HTTP conversation ID is separately restricted in length
and character set, and the final session path is checked during construction.

## Recommended implementation

Add one small internal helper with exact-copy semantics:

```c
static int copy_cstr_exact(char *dst, size_t dst_size, const char *src) {
    if (!dst || !src || dst_size == 0) return 0;
    size_t length = strlen(src);
    if (length >= dst_size) return 0;
    memcpy(dst, src, length + 1);
    return 1;
}
```

The helper must never truncate. Its return value must be checked at every call
site.

Apply it as follows:

1. In `qt_load`, reject an oversized `name` before looking up or reading a
   tensor. Report a clear initialization error and terminate using the same
   failure behavior already used for a missing quantized tensor.
2. Check the return value of the `snprintf` that constructs `wrapped`. Treat a
   negative result or a result greater than or equal to `sizeof(wrapped)` as a
   model-initialization error. Only search for or copy `wrapped` after this
   check succeeds.
3. Copy a successfully resolved `wrapped` name to `nm` with
   `copy_cstr_exact`. This should always succeed because the buffers are the
   same size, but checking it preserves the local invariant.
4. In `teacher_fsync_parent`, keep the existing maximum-length rejection and
   use `copy_cstr_exact` for `path`. Set the no-slash fallback without a string
   copy:

   ```c
   parent[0] = '.';
   parent[1] = '\0';
   ```

5. In `serve_mkdir`, keep the existing maximum-length rejection and use
   `copy_cstr_exact` for `path`. Return failure if the helper fails.

The explicit checks at the call sites should remain even though the helper
also checks the bound. They document the failure policy of each subsystem and
make later refactors easier to review.

## Why not the PR #13 replacement

PR #13 changes each copy to `snprintf(destination, size, "%s", source)` but
does not inspect the return value. That prevents writing beyond the buffer,
yet silently accepts a truncated string. Silent truncation is undesirable for
tensor names and filesystem paths because the program can then look up, read,
create, or synchronize a different name than the caller supplied.

The proposed change is unlikely to break current valid inputs, but it replaces
a memory-safety concern with ambiguous error handling. The better contract is
binary: copy the complete string or fail before using it.

## Portability requirements

- Do not use `strlcpy`; it is available on macOS but is not part of ISO C and
  is not universally supplied by the Linux environments supported by Samosa.
- Do not use `strcpy_s`; it is an optional C11 bounds-checking interface and is
  unavailable in common toolchains.
- `strlen` plus a checked `memcpy` is supported by the existing macOS, Ubuntu,
  and Debian targets.
- Keep format strings constant. When `snprintf` is needed to compose a name,
  always check both formatting failure and truncation.

## Required validation

Before merging the eventual source change:

1. Test exact boundary behavior: empty source, a source of
   `destination_size - 1` bytes, and a source of `destination_size` bytes.
2. Confirm oversized tensor names and paths fail explicitly and do not proceed
   with truncated values.
3. Confirm normal Qwen model initialization and session-directory creation are
   unchanged.
4. Run the complete repository test suite.
5. Run the portable and multithreaded builds on macOS and Ubuntu, plus the
   Debian Bookworm portability test.
6. Rerun the security scanner and confirm no `strcpy`/`strncpy` finding remains
   in `src/qwen36b.c`.

## Pull-request disposition

Do not approve the first-time-contributor workflow solely to validate PR #13.
Either request a revision that implements exact, checked copies on top of the
current `main`, or close that PR and apply the hardening as a first-party
change. In both cases, use the normal review and CI gates above.

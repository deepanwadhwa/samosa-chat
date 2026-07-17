# Local job definitions

[`jss-article-metadata.json`](jss-article-metadata.json) extracts title,
authors, explicitly listed email addresses, affiliations, universities, DOI,
publication month, and editorial dates from the four JSS PDFs at the repository
root. [`jss-article-metadata.expected.json`](jss-article-metadata.expected.json)
is the review reference extracted from those documents.

This job deliberately uses `"unit": "page"`: the articles are long enough
that even the one auto-planned whole-file input is not an appropriate 16-GB
machine-safety test. Page records are reduced deterministically at the end, so
the runner never submits an entire article as one request.

Jobs also enforce an 8,192-token per-inference prefill ceiling by default. It
is independent of the engine's 24,576-token correctness limit: an auto job will
page/chunk a document above that product ceiling, and even `unit:"file"` cannot
bypass it.

The runner uses the installed #5 `samosa-extract` sidecar for page text, exact
Qwen token counts, and bounded page rendering. If a release has no sidecar, it
retains the controlled `extractor_unavailable:application/pdf` review result;
it never falls back to a host-specific PDF tool.

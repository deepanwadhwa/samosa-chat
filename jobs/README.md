# Local job definitions

[`jss-article-metadata.json`](jss-article-metadata.json) extracts title,
authors, explicitly listed email addresses, affiliations, universities, DOI,
publication month, and editorial dates from the four JSS PDFs at the repository
root. [`jss-article-metadata.expected.json`](jss-article-metadata.expected.json)
is the review reference extracted from those documents.

The runner currently defers PDF work with
`extractor_unavailable:application/pdf` until the #5 pdfium sidecar lands. Do
not replace that controlled review result with an ad hoc host-specific PDF tool;
the job is ready to run through Samosa once that extractor contract is available.

# Mixed-folder dogfood evidence

`mixed-folder-2026-09-22.json` records the synthetic acceptance run for the
Chutni and Jobs recovery scope. It captures a successful compiled-gateway
fixture with 30,000 generated-tree decoys, a real searchable PDF extraction,
report/inventory parity, retrieval miss behavior, full-tree undo verification,
and a sampled RSS peak across the gateway and Chutni worker.

The repository suite and focused Jobs, UI setup, and Chutni vendor suites all
passed on the recorded date. The artifact lists individual interruption
recovery outcomes and accurately records that an interactive browser visual
run was unavailable in the current session. The production Jobs JavaScript
submit handler and SSE reader were still exercised with the shipped renderer
and deterministic HTTP/DOM fixtures.

These results qualify only the listed deterministic synthetic fixtures. They
do not qualify a live decision model or every format, language, filesystem,
real-world folder size, concurrent mutation, or full power-loss mode.

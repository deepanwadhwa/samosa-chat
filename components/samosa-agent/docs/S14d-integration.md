# S14d integration — durable Sources cancellation through Agent

C02/C07/C08/C11: qualify a durable document run interrupted while the actual
Sources PDF reader is active. Agent owns conversation/run/child records; Sources
owns extraction and public terminal control. The configured PDFium spy only
slows native page loading and records entry; it does not replace extraction.

Include a standalone native C integration runner and explicit external executable,
PDF fixture and PDFium configuration. Test owner refusal, exact public child
terminal observation, deadline failure, offline retry, preservation of last-good
admitted evidence/assistant and an unrelated healthy reader. Regular/sanitized
Agent runners use independently built Sources/Inference executables. Test commands:
`make test-source-control SOURCES_EXECUTABLE=... INFERENCE_EXECUTABLE=...
PDF_FIXTURE_EXECUTABLE=... PDFIUM_LIBRARY=... PDF_SPY_LIBRARY=...` and corresponding
`sanitize-source-control` target. Missing prerequisites are failures, not passes.
Preparatory integration; App/transport and legacy retirement require later gates.

Concurrent-reader qualification found an extraction lock bug: the PDF snapshot
SELECT remained live throughout native extraction, blocking another reader's
cache commit in SQLite DELETE journal mode. Sources now copies at most its 1 MiB
snapshot limit plus the owned locator, finalizes the statement, and then invokes
the reader. Identity hashing and evidence remain pinned to those immutable bytes.
The test starts a separate page97 read while Agent's delayed reader is active,
then checks that cancellation leaves that exact peer completed and uncancelled.
It also verifies last-good opaque continuation, beyond scope/evidence/assistant.

The deadline fixture originally used600ms; Linux sanitizer startup could exhaust
that before the reader was admitted. It now uses4000ms with four native pages
slowed2000ms each. Acceptance still requires one recorded read child and compares
its observed terminal with public Sources state; startup expiry is not a pass.
This is a test allowance, not a changed production budget.

Linux sanitizer integration reproduced child_failed, preserved at
/private/tmp/samosa-s14d-linux-child-failed.log. Per-helper stderr and ASan reports
are required to diagnose it. Passing subsequent runs do not establish the cause.

Fully instrumented Linux control diagnosis shows individual ASan/LSan helper
calls exceed the unchanged1.5s control-call budget (1.915s and2.005s including
owned teardown), exhausting the3s observation window. That expected resource
behavior must report unconfirmed, rather than claim a terminal response never
received. Strict regular and sanitized-driver integration still require observed
cancelled and exact public terminal agreement. A separate
`sanitize-source-control-unconfirmed` target accepts this additional outcome,
requiring cancellation="unconfirmed", no admitted evidence/continuation changes,
exact parent identity and eventual public terminal (including recovered interrupted
if the owned worker was stopped). Its output records whether unconfirmed was
actually exercised. This is an additional bounded-control scenario, not a pass
for the strict confirmation target. Production deadlines were not enlarged.

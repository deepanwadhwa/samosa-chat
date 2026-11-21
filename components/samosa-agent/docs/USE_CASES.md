# C01 — native product use cases and authoritative owners

Implementation follows the four-folder plan. Every row remains a required product
path unless explicitly optional below; a qualified subset never retires an
unqualified production path. This matrix fixes ownership, not current completion.

| Use case | Public execution path and policy | Authoritative state |
| --- | --- | --- |
| Ordinary chat | App submits Agent chat; Agent budgets semantic messages; Inference counts/generates on explicitly pinned text provider; Agent commits messages/run/events; App renders committed result. | Agent messages/run/revision; Inference artifact/session; App draft/theme only. |
| Selected text/PDF question | App registers through Sources and submits explicit selector to Agent document policy; Sources reads qualified range; Agent admits literal version once, budgets and calls Inference; Agent commits answer/citation/evidence. | Sources immutable bytes/extraction/cache; Agent selected scope/admitted copy/history; Inference private continuation. |
| Hot/cold/compact document follow-up | Agent replays copied qualified evidence and typed history through one policy; public Inference session reuses/restores provider continuation; explicit Agent compaction retains originals and starts zero extraction. | Agent semantic history/derived summary/public ref; Inference opaque recovery/cache. |
| Folder inventory/query | Sources authorizes selected root, returns bounded read-only inventory with completeness; Agent folder policy selects/reads qualified candidates and answers within inspected coverage. | Sources locators/inventory/index/checkpoints/evidence; Agent selected scope/task result. |
| File finding | Agent candidate→inspect→verify→select pipeline calls Sources inventory/retrieval/preview; semantic scoring is replaceable inside this pipeline and must be corpus-evaluated. | Sources exact candidates/evidence/coverage; Agent bounded selection policy/result. |
| Explicit organization by type | Agent asks Sources inventory, deterministically makes immutable checked copy/move plan, waits for approval bound to plan revision/identity, applies through one executor and records receipts; Sources reconciles locators. | Agent plan/approval/intent/receipt/run; Sources updated locator/index freshness. |
| Copy/move/undo | Every interactive or scheduled trigger uses the same Agent checked no-clobber executor. It records intent before effects, reconciles interruption and refuses undo over an unrelated destination. | Agent durable effect receipts and authorization; host filesystem actual effect; Sources immutable old citation plus current locator. |
| Scheduled authorized action | Agent durable policy names exact future authorization/scope; trigger submits to the same executor. One-time approval gives no future authorization. | Agent schedules/policy/plan/receipt/events. |
| Background indexing during chat | Sources owns bounded resumable work/checkpoints; optional enrichment calls Inference background admission alongside interactive chat under one fair resource policy. | Sources indexing/exact evidence/derived artifacts; Inference host scheduling/residency; Agent conversation independent. |
| Web research | Sources fetch/search returns bounded provenance-qualified evidence; Agent web policy selects/inspects and cites it. External adapters are explicitly configured. | Sources captured response/evidence/coverage; Agent research policy/result. |
| Cancellation and reconnection | Owner-scoped operation IDs target resources; observers reconnect from ordered committed events. Run lifetime/detach policy is explicit; unrelated workers/sessions remain alive. | Each component owns its operations/resources; Agent owns run/event truth. |
| Application launch/stop | App authenticates ingress, displays descriptors and starts/stops only recorded instances it owns. Already-running headless components remain independent. | App presentation/session/owned-instance records; each component retains its own durable truth. |

Default supported native inference is one configured compatible llama-server text
provider with arbitrary public provider ID and pinned model alias/context. Current
native quality fixture uses Ornith; family/model naming never enters Agent or App
policy. The fake provider is contract-test infrastructure, not product quality.
A minimal installation reads deterministic sources and opens App without weights;
missing optional inference reports unavailable while reading remains usable.

HTML/DOCX/PDF/OCR source reading and bounded image/audio preparation belong in
Sources. System PDFium/Tesseract/FFmpeg are optional declared native dependencies,
not a mandatory language-model installation. Neural STT/TTS/vision/classification
and summarization adapters are optional and cannot claim supported status before
separate real-engine gates pass. Unsupported descriptors/requests are explicit.
Browser microphone/playback/TTS may remain App capabilities. Chutni is an existing
optional configured storage adapter behind Sources, never shared task policy.

Allowed cross-owner access is exclusively public JSON calls, declared native
provider ABI/HTTP, and qualified immutable evidence copies in Agent. No component
opens another owner's database, chat directory, extraction cache or checkpoint.
Sources/Inference build without Agent/App; read-only inventory imports no checked
executor. Agent owns prompts/task policy; App owns assembled compatible-version
manifest and launcher, not a fifth coordinator. S01–S08 are bounded replacement
subsets; remaining use cases, assembled quality/retirement and full C01–C14 gates
stay explicit in the verification plan.

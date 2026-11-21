# Agent file-finding contract v1

`find_files` accepts contract_version1,job_id64 lowercase hex,query nonempty UTF-8
<=128 bytes,max_inspect1..3. Host configures Sources argv; request cannot choose
commands, owner, state or host. No Inference or filesystem effects required.

One shared candidate -> inspect -> verify -> select pipeline. Request public
index_search (<=10 candidates), inspect <=3 ordered candidates with read_text
using exact source/version/byte range. Shared document evidence validator checks
literal contract, source/version/range/length/coverage/reader, then finding checks
candidate evidence identity/coverage and actual literal substring. Preview text
never proves a match. Strategy explicit_literal_match uses ASCII case-insensitive
matching; UTF-8 text is preserved. Semantic ranking is not implied.

Return selected locators/mechanical identities with exact literal evidence,
index coverage/freshness, candidate/inspected/uninspected/verified counts and
selection_coverage. Complete means only returned indexed candidates were examined.
exhaustive_current_filesystem is always false; zero verified results makes no
current filesystem absence claim. Selection does not authorize file actions.

Request<=16KiB, Source response<=1MiB and10s per bounded helper call; at most4
Source calls (one search,three inspections); selected payload<=768KiB. No output
is published before all selected inspections validate. On failure, one typed
error replaces selections. Unknown fields and invalid bounds refuse before work.
Transport ownership/death/deadline behavior is the existing native guardian.

Question and finding share src/evidence.c, so equivalent literal validity checks
apply across both policies. Sources remains authoritative for snapshot/index
storage; the Agent never opens it. Finding is currently an explicit headless
operation; durable task run integration and semantic strategies are later scopes.

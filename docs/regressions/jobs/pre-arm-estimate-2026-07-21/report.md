# Pre-arm Estimate Checkpoint

Generated: 2026-07-21T19:13:28.592422+00:00

Scope: CLI smoke for `suggest-job --out` followed by `estimate`. No model server was invoked.

Unit count: 3
Model units: 3
Input tokens: 53 (exact=False)
Output tokens: 1536
Estimated wall: 4m 19s (258.6s)
Battery policy: run manually; daemon battery policy is not active yet

Acceptance: PASS for the implemented slice - estimate is available before run/arm, reports unit count and projected wall-clock. Token counts are marked conservative when no exact token counter is injected.

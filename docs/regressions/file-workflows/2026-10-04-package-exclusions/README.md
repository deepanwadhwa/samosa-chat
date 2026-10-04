# Package exclusion correction — 2026-10-04

The screenshot exposed a real policy mismatch: Samosa's metadata inventory
already pruned application bundles, but Chutni's reference indexing scanner
did not. It therefore cataloged and hashed package internals, which the native
enrichment pipeline then sampled and summarized. The previous throughput
fixtures did not cover this parity failure.

The user's memory build was canceled through its control endpoint, then the
runtime was stopped to terminate its in-flight work. No Downloads directory,
file, adjacent portable store, or existing conversation was opened for
inspection or testing. Only application control state was checked.

## Correction

- Both scanners prune `.app`, `.bundle`, `.framework`, `.plugin`, `.xcodeproj`,
  `.xcworkspace`, and `.photoslibrary` entries, case-insensitively, before
  enumerating their contents. The host's existing exclusions are retained.
- The enrichment boundary rejects those path components before stat, hashing,
  extraction, OCR, or summarization, including entries in older catalogs.
- When a scan encounters a formerly indexed subtree that is now excluded,
  its recorded descendants are marked excluded using catalog hierarchy alone.
  This does not enumerate the subtree or inspect its files.
- Ordinary search filters excluded sources, so retained legacy artifacts do
  not return as current evidence. Explicit stale inspection labels them unknown
  without checking their excluded file paths. Freshness checks also avoid
  opening sources already marked excluded.
- The app policy version advances to 2 and the inventory fingerprint to v3.
  New scope metadata and preflight exclusions list these package patterns.
  Existing indexes are recognized as using the earlier policy by preflight.
- Restart recovery preserves a pending cancellation as canceled; interrupted
  running jobs retain the existing paused recovery behavior.
- Canceled, paused, and completed cards hide stale worker rows and current-file
  activity left in saved progress. Historical counters remain visible.

## Generated regression coverage

- Native inventory fixture checks every package suffix, including uppercase
  `.APP`, and verifies no descendant file is emitted.
- Chutni core fixture checks the same suffixes, retains an ordinary neighbor,
  seeds a simulated legacy package and file, rescans, and verifies both entries
  are excluded and old content does not appear in normal search.
- Gateway fixture puts package resources beside five ordinary documents.
  Preflight and the completed index both retain the same five documents;
  policy version and visible exclusions are checked.
- Reader guard checks nonexistent package paths: they are excluded without
  attempting the reader and without creating failures or outputs.
- Restart test distinguishes canceling from running jobs.

Inventory, Chutni core/MCP/binding, gateway, pause/resume, crash recovery,
sampling, and native concurrency/atomic-storage tests pass. The wider FW-6
acceptance work and scheduling gate remain open. This change does not inspect,
repair, refresh, or resume the user's folder.

Installed release `dev-08d1031a8ca7` is running and reports ready. Its gateway,
inventory, Chutni service, and UI hashes match the tested files; served HTML
matches after the expected token injection. The known user scope is confirmed
`canceled_initial`, and pending automatic memory jobs are deferred.

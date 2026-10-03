# Walkthrough feedback, 2026-10-03

The owner reported a blue inner ring when clicking the chat composer, an inaccessible temporary fixture location, duplicate folder-question buttons, and an incorrect “unspecified” safety-limit error on the partial-memory card.

## Repairs and verification

- The composer uses its existing neutral outer border for focus; its textarea has no inner focus outline. Other keyboard targets retain visible neutral outlines. A real Chrome pointer click reported `outline: none` and composer border `rgba(0, 0, 0, 0.24)`.
- Partial memory now distinguishes file-reading failures from explicit safety-budget limits. The prior synthetic fixture intentionally had an unreadable file: its protocol snapshot reported `errors: 1`, `partial: false`, `complete_for_policy: false`. This was a reading failure, not a safety limit. The new card accurately reports one incomplete file and does not claim it is active in an unrelated conversation.
- The rendered partial card contains exactly one Ask button. Chrome checked this count and the corrected explanation through the actual app.
- The walkthrough source is now `~/Documents/Samosa Walkthrough/Files`, accessible through Finder and ordinary folder pickers. It has the 12 relevant files and eight distractors. Deliberately unreadable/unsupported/symlink cases are separately labeled under `Coverage Cases`.
- A fresh isolated walkthrough app runs on port 18642. Automatic memory activation inspected all 20 ordinary files with no safety limits and reached `ready`. Prior automated fixture state is preserved in its previous isolated home rather than deleted.

`node tests/test_chutni_ui.mjs`, `node tests/test_sidebar_ui.mjs`, and `node tests/test_jobs_ui.mjs` pass. Presentation regression cases distinguish reading errors, actual safety limits, and incomplete coverage without a supplied reason. Screenshots record the repaired composer and original partial fixture card. These feedback fixes do not claim completion of the human walkthrough or a new full-suite pass. Long-document and final full-suite acceptance still require resolution of their latest failures; background model tests are held during the owner’s walkthrough.

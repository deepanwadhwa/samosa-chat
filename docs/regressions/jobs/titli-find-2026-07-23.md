# Find-job dogfood failure #2 — "Titli" against a real ~/Downloads (2026-07-23)

Owner ran a find job through the live app (Ornith backend, Jobs tab):

> Folder: `/Users/deepanwadhwa/Downloads` (510 files: 271 PDFs, 82 images,
> 48 text, 109 other)
> Task: *"Can you find all files pertaining to my cat's medical records?
> my cat's name is Titli"*

Run 1: 8 tool calls (4× `fs_metadata`, 4× `fs_read_pages`), then the job asked
**"What is your pet's name?"** — a name the task already contained. Owner
answered "Titli". Run 2: no file was inspected at all; the job concluded from
filenames that nothing matches and ended by asking whether it should ask
questions.

This is the second dogfood failure of this scenario (first: 2026-07-21, logged
in TASKS_JOBS.md Phase JF preamble, which motivated the JF spec). This one is
worse: the JF *implementation* failed in ways the JF *spec* explicitly warned
against.

**Scope note:** analysis is from source only. No file in the owner's
~/Downloads was opened, and no job artifacts that could embed extracted
document text were read. Filenames referenced below are synthetic.

## Root causes (five, compounding — all live at `ad825d1` on `issue-7-jobs`)

### RC1 — delimiter-escaping bug in `candidate_score()` destroys the goal's search terms

`src/samosa_gateway.c:1851` tokenizes the goal with the C literal
`" \\t.,?!:;/\\\"'()[]{}"`. `\\t` is a backslash **plus the literal letter
t** — not a tab. Every goal word containing "t" is split at the "t".

Reproduced with the function copied verbatim into a standalone program
(`score_test.c`, below). Tokens derived from the owner's exact goal:

```
[Can] [you] [find] [all] [files] [per] [aining] [o] [my] [ca] [s]
[medical] [records] [my] [ca] [s] [name] [is] [Ti] [li]
```

- "Titli" → `Ti` + `li`, both under the 3-char minimum → **discarded**.
- "cat's" → `ca` + `s` → **discarded**. The word "cat" never becomes a token.
- "records" is a hardcoded stopword (`src/samosa_gateway.c:1848`).
- Surviving junk fragments substring-match filenames: `Can` matches
  "**Can**" in "CamS**can**ner", `all`/`per` match "w**allp**a**per**",
  `you` matches "medicare_and_**you**".

Scores on synthetic names (full program output):

```
 3  Titli vaccination record 2023.pdf   <- ideal target file
 5  titli_vet_visit.pdf
 4  CamScanner 03-15-2024 14.22.pdf     <- anonymous phone scan
 4  CamScanner 11-02-2023 09.10.pdf
 4  medicare_and_you_2024.pdf
 9  medical_coding_reference.pdf
 8  wallpaper_gallery.zip
 4  training_schedule.txt
```

A file **named after the cat scores below a wallpaper archive**.
`build_candidates()` keeps the top 40 by score, so with 271 CamScanner-style
scans at score ≥4, a perfectly named real record at score 3 is **excluded
from the candidate list entirely**. This exactly reproduces the junk
candidates the owner saw (Medicare, medical-coding, CamScanner files).

### RC2 — the clarifying question is a canned C template

`src/samosa_gateway.c:2469-2471`: when the 8-round loop budget
(`src/samosa_gateway.c:2369`) is exhausted, the gateway prints *"I could not
identify the right record yet. What is your pet's name?"* whenever the goal
contains the substring "cat" or "pet" — with no check that a name is already
present, and no model involvement. Run 1 used exactly 8 rounds, so this fired.
(`contains_case` means a goal about "edu**cat**ion" would also be asked for a
pet's name.)

### RC3 — the answer could not have helped

`jobs_answer` (`src/samosa_gateway.c:2588`) appends
`"\nAdditional detail from the user: Titli"` to the goal and **restarts the
job from scratch** via `jobs_report`. RC1 then shreds "Titli" again — the
answer is mathematically incapable of changing the candidate list.

### RC4 — resume amnesia

`save_job_state` (`src/samosa_gateway.c:435`) persists only
`{job_id, goal, folder}`. The run-1 conversation — all 8 inspections — is
freed on pause. JF.3 in TASKS_JOBS.md **explicitly specified** persisting the
loop conversation to `<job_dir>/convo.json` and re-entering the loop with the
answer appended as a tool result, and flagged this as "the phase's only real
mechanism risk". The shipped code does not implement it.

### RC5 — scanned PDFs were read with a tool that cannot see them

The find-loop system prompt (`src/samosa_gateway.c:2356`) instructs *"Read
PDFs only with fs_read_pages"*, which calls `samosa-extract --json-pages` —
**text-layer extraction only, no OCR** (`src/samosa_extract.c:742`).
CamScanner-style phone scans have no text layer, so every "Document pages
read" step returned empty text. `doc.read` — the R1–R7 tiered OCR cascade
built for precisely these files — is present in the tool list
(`src/samosa_gateway.c:2332`) but the prompt steers the model away from it,
and the run's event stream confirms it was never called (UI strings map
`fs_metadata`/`fs_read_pages` only — `assets/app.html:836,841`).

## Secondary defects

- **UI overclaim:** "Checked 510 filenames" — the model saw at most 40
  gateway-selected names (`assets/app.html:969-971`; `build_candidates` cap).
- **Final-answer guard** (`src/samosa_gateway.c:2387`) accepts any content not
  containing `fs_` / `samosa_tool`, so run 2's "Would you like me to: Ask
  you…" question-shaped non-answer was accepted as the job result.
- **Task/architecture mismatch:** the goal is a sweep ("find **all** files");
  `jobs_find` is a single-file finder (one staged `fs_move`, singular answer,
  8 probes for 271 PDFs).
- **Zero find-path test coverage:** `tests/test_compiled_gateway.sh` never
  exercises `jobs_find`; no planted-file scenario, no resume test, no
  assertion against canned questions. `candidate_score` shipped untested.

## Spec-vs-implementation divergence (Phase JF, TASKS_JOBS.md)

| JF spec said | Shipped code does |
|---|---|
| model filters candidates metadata-first, composing low-level tools freely | C `candidate_score()` pre-filters to 40 names with keyword shenanigans (nowhere in spec) |
| JF.3: persist convo to `<job_dir>/convo.json`; answer re-enters the loop as a tool result | `job.json` = goal+folder only; answer restarts the job with a mutated goal |
| `ask_user` is a model tool for genuine ambiguity | plus a hardcoded C fallback question keyed on substrings "cat"/"pet" |
| JF.4: verification gate = real model finds a real planted file, evidence under `docs/regressions/jobs/` | no such run recorded; this failure is the first artifact in that directory |
| v1 strictly read-only; find→move is v2 | `fs_move` staging shipped inside the find loop |

## Reproduction program

`score_test.c` — `contains_case`, `candidate_score`, `path_copy` copied
verbatim from `src/samosa_gateway.c` (lines 1804–1810, 1845–1867, 99–102)
plus a `main()` that prints the token list and the score table above.
Compiled with `cc -O2` on the reference M3 (Darwin 25.5.0), output pasted
above verbatim.

## Disposition

Fix program: [../../TASKS_JOBS_INTELLIGENCE.md](../../TASKS_JOBS_INTELLIGENCE.md)
(Phase JI). Registered in CLAUDE.md open defects as **J11**.

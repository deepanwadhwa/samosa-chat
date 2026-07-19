# Samosa UI — design review and design language

Owner ask (2026-07-19): *"super professional, utilitarian, minimal, something
new and modern… it should feel like Apple designed it. Smart, elegant,
ingenious. Not LLM slop."* This document is (1) an honest review of the
current app UI, (2) the design language that fixes it, and (3) the visual
spec for the Jobs view ([TASKS_JOBS.md](TASKS_JOBS.md) J1.12 / §JO.6 / J2).

The decided product surface is **browser + headless server** (recorded in
[TASKS_JOBS.md](TASKS_JOBS.md) §Decisions locked, 2026-07-19): no
dmg/Electron/exe shell. This page is therefore the entire visual identity of
the product.

## 1. Review of the current UI ([assets/app.html](../assets/app.html))

### What is genuinely good — keep it

- **A real point of view.** The warm paper-and-spice palette
  ([app.html:9-25](../assets/app.html#L9-L25)) is distinctive; almost every AI
  chat app is grey/indigo. The mascot, the name, the warmth — this is an
  identity, not a template. Keep the temperature; discipline the application.
- **Honest microcopy.** "Running only on this computer"
  ([:253](../assets/app.html#L253)), "responses stay on this Mac"
  ([:282](../assets/app.html#L282)), "ready on this Mac", the settings copy
  that admits longer ceilings cost time and SSD reads
  ([:310](../assets/app.html#L310)). This is the project's evidence culture as
  interface copy. It is rare and it is the brand. Extend it; never let a
  redesign make the copy vaguer.
- **The telemetry footer** ([:285-290](../assets/app.html#L285-L290)) — Mode /
  Speed / Memory / Closure. No hosted chat app shows you tokens-per-second and
  resident memory. Utilitarian, honest, differentiating. Promote it, don't
  bury it (see §2.7).
- **Thinking as a disclosure** ([:158-160](../assets/app.html#L158-L160)),
  system font stack, dark mode via `color-scheme` + variables, `aria-label`s,
  escaped interpolation, the follow-output/jump-to-latest logic — solid bones.

### What reads as generated-UI — the specific tells

1. **Decorative radial color washes** on the body background
   ([:51-54](../assets/app.html#L51-L54)). Two glowing blobs behind the
   content is the single most recognizable AI-generated-UI trope. Apple
   surfaces are flat or subtly material — never vibed.
2. **Gradient CTA with a colored glow** — `linear-gradient(135deg, brand,
   #d96d30)` + `box-shadow: 0 8px 22px rgba(166,71,29,.20)`
   ([:88-89](../assets/app.html#L88-L89)). Landing-page grammar, not tool
   grammar. A primary button in a professional tool is a flat accent fill.
3. **Shadow inflation.** `0 20px 60px` on the composer
   ([:23](../assets/app.html#L23)), `0 7px 26px` under every assistant bubble
   ([:144](../assets/app.html#L144)), `0 8px 24px` on a pill
   ([:129](../assets/app.html#L129)). Everything floats, so nothing does.
   Apple uses hairlines for structure and reserves shadow for true overlays.
4. **Hover-lift on everything** — `translateY(-1px)`
   ([:91](../assets/app.html#L91), [:137](../assets/app.html#L137)). Buttons
   are not balloons; hover should tint, not levitate.
5. **No radius system.** 22, 18, 15, 14, 13, 12, 11, 10 px all appear
   ([:24](../assets/app.html#L24), [:143](../assets/app.html#L143),
   [:136](../assets/app.html#L136), [:86](../assets/app.html#L86),
   [:154](../assets/app.html#L154), [:99](../assets/app.html#L99),
   [:158](../assets/app.html#L158), [:120](../assets/app.html#L120)). Radius
   chaos is invisible consciously and visible instantly as "not designed".
6. **Glyph soup for icons** — fullwidth `＋` ([:250](../assets/app.html#L250)),
   `☰` ([:259](../assets/app.html#L259)), `↑` `■` ([:278-279](../assets/app.html#L278-L279)),
   `✕`/`×` ([:272](../assets/app.html#L272), [:447](../assets/app.html#L447)).
   Mixed Unicode glyphs render with different weights and optical sizes per
   platform. Apple-feel requires one drawn icon set.
7. **Blur as default material.** `backdrop-filter` on five surfaces
   ([:71](../assets/app.html#L71), [:117](../assets/app.html#L117),
   [:129](../assets/app.html#L129), [:169](../assets/app.html#L169),
   [:180](../assets/app.html#L180)). Materials mean layering; when everything
   is glass there is no hierarchy (and compositing cost on a machine we are
   trying to keep cool).
8. **Micro-type below legibility** — 10px telemetry and composer hint
   ([:184-186](../assets/app.html#L184-L186)); and metric numbers without
   `tabular-nums`, so Speed/Memory jitter in width on every 5 s health poll.
9. **Weight scatter** — 650, 680, 700, 800 font weights
   ([:89](../assets/app.html#L89), [:121](../assets/app.html#L121),
   [:76](../assets/app.html#L76), [:142](../assets/app.html#L142)) with no
   scale behind them.
10. **IA wart:** Internet source lives inside *Settings*
    ([:317-324](../assets/app.html#L317-L324)). Fetching a source is an act of
    composing a message, not a preference. It belongs at the composer (a `+`
    menu: image / web page / document — which is also where #5's document
    attach will land).

Net judgment: the current UI is a **good draft with the right values and an
undisciplined surface**. The fix is subtraction and systemization, not a new
concept.

## 2. The design language

One sentence: **a quiet, warm instrument — paper, hairlines, one spice-orange
accent, and numbers you can trust.** The model is the product; the UI is the
lab bench it sits on. "Ingenious" here means the honesty *is* the aesthetic:
real telemetry, real states, real provenance, beautifully typeset — things no
hosted competitor can show.

### 2.1 Principles

1. **Quiet until it matters.** Neutral surfaces, hairline structure. Color
   appears only as the one accent (primary action, focus, links) and as
   semantic status. If everything whispers, the accent can speak.
2. **Structure from lines, not shadows.** 1px low-alpha borders separate
   regions. Shadow exists at exactly one level: true overlays (settings sheet,
   menus, dialogs).
3. **Numbers are first-class citizens.** Every metric is `tabular-nums`, every
   path/hash is the mono stack, columns never jitter. This is the "feels
   engineered" signal.
4. **Honest states, designed.** "Paused — you're chatting", "Ready on this
   Mac", "Snapshot could not be saved" are designed states with iconography
   and tone, never afterthought toasts.
5. **Warmth is the identity, restraint is the craft.** Keep the paper tint and
   the mascot; delete the washes, gradients, and glow.

### 2.2 Tokens (replace the ad-hoc values)

```css
:root {
  /* surfaces */
  --paper:    #faf7f2;   /* app background — flat, no gradients */
  --surface:  #ffffff;   /* cards, composer, inputs */
  --inset:    #f1ece3;   /* wells, code blocks, thinking */
  --line:     rgba(60, 42, 28, .12);      /* the only border */
  --line-strong: rgba(60, 42, 28, .22);   /* focused/active border */

  /* ink */
  --ink:      #221a13;
  --ink-2:    #6f6459;   /* secondary text — AA on --paper */
  --ink-3:    #9a8d81;   /* tertiary: hints, timestamps */

  /* the one accent */
  --accent:       #b3501e;
  --accent-ink:   #fff6ef;  /* text on accent */
  --accent-soft:  rgba(179, 80, 30, .10); /* selected/hover tint */

  /* semantic status — used only for status */
  --ok:      #3d8f5f;
  --warn:    #b07d2c;
  --danger:  #b0392e;

  /* elevation: exactly two levels */
  --shadow-overlay: 0 8px 30px rgba(34, 22, 12, .16);
  /* level 0 = none. Cards get --line, not shadow. */

  /* radius scale: three values, no exceptions */
  --r-sm: 6px;    /* chips, small controls, code spans */
  --r-md: 10px;   /* buttons, inputs, cards, bubbles */
  --r-lg: 16px;   /* composer, overlays, sheets */

  /* type */
  --font-ui:   -apple-system, BlinkMacSystemFont, "SF Pro Text", "Segoe UI", sans-serif;
  --font-mono: ui-monospace, "SF Mono", SFMono-Regular, Menlo, monospace;
}
```

Dark theme mirrors the same roles (deep warm brown-blacks `#161210` /
`#1e1915`, ink `#f4ece3`, accent `#e08a52`); tokens change, rules don't.

### 2.3 Type scale

| Role | Size/line | Weight | Notes |
|---|---|---|---|
| Display (welcome h1) | 28/34 | 700 | tracking −0.02em — the only tight tracking |
| Title (panel headers) | 17/24 | 600 | |
| Body / messages | 15/24 | 400 | |
| Secondary (list items, labels) | 13/18 | 400–500 | |
| Caption (hints, timestamps) | 12/16 | 400 | **nothing below 11px, ever** |
| Metric | 13/16 | 500 | `font-variant-numeric: tabular-nums` |
| Mono (paths, hashes, code) | 12.5/20 | 400 | `--font-mono` |

Three working weights (400 / 500–600 / 700). Delete 650/680/800.

### 2.4 Space, layout, materials

- 4px base grid; component padding in 8/12/16/20; section gaps 24/32.
- Content column stays ~820px; sidebar 280px fixed, `--paper` with a hairline
  — **no blur**. Blur is allowed in exactly two places: the top bar over
  scrolling content, and overlays.
- Flat `--paper` body. Delete both radial washes. Warmth comes from the paper
  tint and the accent, not atmosphere.

### 2.5 Components (the deltas that matter)

- **Primary button:** flat `--accent`, `--r-md`, weight 600, no gradient, no
  glow. Hover darkens ~6%; press `scale(.985)`; focus ring
  `0 0 0 3px var(--accent-soft)` + `--line-strong`.
- **Secondary/icon buttons:** transparent, hairline on hover surfaces only,
  tint `--accent-soft` when active. No lift anywhere.
- **Icons:** one inline SVG set, 16/20px grid, 1.5px round stroke,
  `currentColor`: plus, sidebar, arrow-up (send), stop-square, x, gear,
  folder, doc, photo, clock, check, alert. Replaces every Unicode glyph.
- **Bubbles:** user = `--inset` with `--ink` text (drop the near-black slab —
  it's the heaviest element on screen and it's the *user's own* words);
  assistant = `--surface` + hairline, **no shadow**. Both `--r-md`; keep the
  small tail-corner detail. Max width unchanged.
- **Status dot + label:** one component, colored strictly by `--ok/--warn/
  --danger`, label set in Secondary. Reuse in top bar, jobs, sidebar footer.
- **Composer:** `--surface`, hairline, `--r-lg`; focus-within raises border to
  `--line-strong` + soft accent ring. The `+` becomes a small menu (Image /
  Web page / Document) — this is where the Settings web card moves, fixing
  §1.10 and giving #4/#5 their attach surface for free.
- **Motion:** 120–180ms ease-out, opacity/transform only. Message arrival
  fade+2px rise stays. Respect `prefers-reduced-motion`.

### 2.6 Voice

Sentence case everywhere (drop the all-caps `SECTION-LABEL` tracking-wide
style). Numbers with units, states in plain language, no exclamation marks.
The honest-copy rule from [CLAUDE.md](../CLAUDE.md) applies to pixels too:
never a spinner that pretends to know progress it doesn't — indeterminate
work gets the two-clock treatment (below), not a fake percentage.

### 2.7 The status bar (evolved telemetry footer)

Left-aligned, 12px caption + 13px tabular metrics, hairline top border:

```
● Ready on this Mac    Direct · 6.8 tok/s · 3.9 GB · natural close
```

One line, stable columns, dot = the status component. During generation the
speed value ticks live; nothing else moves. This bar is shared chrome across
Chat and Jobs.

## 3. The Jobs view

Three constraints define it, in priority order: **the bakery test** (below),
J1.12's static contract (self-contained HTML, inline CSS, **no JS**,
everything escaped), and the product truth that **the morning review is the
product**. Everything below is expressible in pure HTML/CSS (`<details>`,
CSS bars).

### 3.0 The bakery test (owner rule, 2026-07-19 — overrides everything below it)

> Someone who runs a small bakery and is not tech-savvy sees this page for
> the first time. They must understand it. Extremely simple means extremely
> simple.

Concretely:

- **The first screen is sentences, not data.** What happened, what needs
  them, where their files are, and that nothing was deleted — in plain
  words. "Your June receipts are sorted. 69 moved into folders by day,
  3 need a quick look." Numbers appear inside sentences, not as a dashboard.
- **One idea per section, ≤ 4 sections before the fold**, in this order:
  the message → needs a look → where your files are → the safety card
  (never deleted + Undo).
- **Plain-language reasons, always.** `missing_required_field:date` is a log
  string; the page says "No date is visible on this receipt." The J1.5 error
  taxonomy never appears on the first screen — it lives in the JSON and the
  Details section. Every reason must be writable as one sentence a
  non-technical person acts on ("Open it").
- **Everything technical collapses into one `<details>` — "Details for the
  record":** timing (the two clocks, prose form: "6 h 52 m in total, 3 h 41 m
  of model reading"), the full per-move list (dimmed common prefixes,
  `--font-mono`), the per-unit table (hash prefixes, granularity, timings),
  the exact undo command, and the provenance line (run fingerprint, seed,
  engine build, tokenizer, event count). The evidence culture is preserved
  in full — one click away, never in the reader's face.
- **The jargon blacklist for the first screen:** unit, inference, prefill,
  schema, provenance, hash, granularity, JSONL, event, `review_required`.
  Allowed: files, receipts/photos/documents (name the actual thing the job
  ran on), folders, moved, skipped, "needs a look", undo.

### 3.1 First screen (top to bottom)

1. **The message.** Status dot + one caption line (`Samosa · Receipts — June
   2027 · finished Saturday 4:55 am`), then a Display-size headline stating
   the outcome ("Your June receipts are sorted."), then 2–3 sentences with
   the counts. While running, the same shape: "Sorting your receipts — 32 of
   74 read so far", one hairline `--accent` meter, and the honest pause
   states in plain words ("Paused while you're chatting — it will continue
   on its own"). Status chips with icon + label for review/failed counts
   (never stacked warm bar segments — adjacent accent/warn/danger fills fail
   color-vision separation, measured: ΔE 7.3 deutan / 10.6 normal).
2. **Needs a look** — always before successes. One card per file: filename,
   one plain sentence why, one action ("Open it"). In J2 this becomes the
   side-by-side review (source image beside extracted fields, per-field
   confirm/edit; keyboard J/K/Enter/E).
3. **Where your files are** — the destination path as a breadcrumb
   (`Documents › Receipts › Organized`) and a short folder list with counts
   ("June 5 — 9 receipts"), collapsing the tail ("4 more days — 35
   receipts"). Skipped files get one plain sentence, ending "Nothing was
   overwritten."
4. **The safety card** — always visible, never inside Details: "**Nothing
   was deleted.** Files were moved, never copied or removed. Undo puts all
   69 back exactly where they came from." plus the Undo affordance (button
   in J2; in static v1 the button anchors to the Details section showing the
   exact command).

### 3.2 Pre-apply state (the plan)

Same shape, different message: headline "Here's the plan — nothing has been
moved yet", the folder list shows *planned* counts, the safety card swaps
Undo for Apply, and the skips are listed with their plain sentences. The
full src → dst manifest lives in Details for the record, grouped by
destination folder with the unchanged path prefix dimmed to `--ink-3` so the
eye reads only what changes.

### 3.3 Job list (J2 interactive view)

Cards in a single column: name, intent chip, status, mini two-clock, thin
progress bar, `Updated 12:41`. Sort: running, paused, review-pending,
completed. The chat sidebar gains one quiet `Jobs` item with a count badge
only when review > 0 — jobs whisper; they never interrupt a conversation.

### 3.4 What the Jobs view never does

No emoji as status, no fake spinners, no red for `review_required` (it is
normal operation — `--warn`), no truncated paths without full value in
`title`, no unescaped anything (J1.12 test), no external fonts/assets ever.

## 4. Order of work

1. **Tokens + subtraction pass on app.html** (washes, gradients, shadows,
   lifts, radius/weight normalization, 10px→12px, tabular-nums). Pure CSS,
   zero behavior change, one afternoon of careful diffs.
2. **SVG icon set** replacing glyphs (touches markup, still no logic).
3. **Composer `+` menu** — moves web fetch out of Settings; the attach seam
   for #4/#5.
4. **Jobs static view** (J1.12 + §JO.6) styled per §3 — first shipped surface
   of the full language.
5. **J2 interactive Jobs view + side-by-side review** when the daemon lands.

A working mockup of §3 lives at [mockups/jobs-view.html](mockups/jobs-view.html)
(self-contained, zero JS, follows light/dark automatically — open it directly
in a browser); treat it as the acceptance target for step 4.

# E-J1 JSS PDF Batch - Ornith + PDFium (2026-07-22)

This is the full labeled JSS four-PDF batch through the compiled gateway
definition routes, using a PDFium-backed installed `samosa-extract` and the
local Ornith 1.0 9B backend.

## Inputs

- Job: `jobs/jss-article-metadata.json`
- Labels: `jobs/jss-article-metadata.expected.json`
- Inputs used for the run: `/tmp/samosa-ej1-jss-inputs-20260722`
- Files: `v109i02.pdf`, `v109i03.pdf`, `v110i01.pdf`, `v110i02.pdf`
- Runner: `tools/run_e_j1.py`
- Gateway route: `/v1/jobs/definition/preview`, then `/v1/jobs/definition/run`

The input directory was intentionally a four-file temp folder, not `~/Downloads`;
`~/Downloads` contained unrelated PDFs and would not be a labeled E-J1 batch.

## PDFium Artifact

The installed extractor was built from the reviewed PDFium macOS arm64 archive:

- Release source: https://github.com/bblanchon/pdfium-binaries/releases/tag/chromium%2F7961
- Local unpacked dir: `/Users/deepanwadhwa/Documents/samosa-pdfium-artifacts/chromium-7961/mac-arm64-unpacked`
- `pdfium-mac-arm64.tgz`: `1193a771e0bd934530afa3df73a0d44551d8f4078442e290054e6dd38ded960f`
- `pdfium-linux-x64.tgz`: `019665c8877d46fe65f625f80fd714ab07aac68554b0636acf2a2adf9288adb2`
- `pdfium-linux-arm64.tgz`: `974107999784a438149605024475d42d80dd306799d90e1af5f6fa63f976455f`

Install used `tools/install_local_dev.sh` after rebuilding with `PDFIUM_DIR`.
The installed dev release was `dev-19a7cadf2281`.

## Result

- Preview: 21.888 s
- Run: 135.149 s
- Terminal units: 4
- Status counts: `passed=4`, `review_required=0`, `failed=0`
- Field accuracy: 46/48 = 0.9583
- Misses:
  - `v109i02.pdf`: `author_emails` order
  - `v110i02.pdf`: `affiliations` included an extra research institute
- Memory safety: `Pages throttled=0`, `Swapins=0`, `Swapouts=0` before and after
- Power/thermal: AC power; no thermal or performance warning recorded

## Interpretation

This closes the previously open PDFium/install/document-support path for the
labeled JSS PDFs and replaces the earlier page-1 workaround. It does not close
all of E-J1: there is no labeled image corpus in this branch, Ornith reports
`supports_images:false`, and the interactive chat interlock still needs a
separate acceptance run.

Follow-up after this run: the compiled definition route now emits
`model_call_seconds` / `active_inference_seconds` and has an offline
pause/resume interlock regression. This evidence directory remains an accurate
record of the earlier JSS run, so its `report.json` still says active inference
was not reported. Rerun E-J1 to populate those fields in live PDF evidence.

# Local CI before publishing

Source revision: `b41886f` (no push performed while any required local gate was pending).

The checks reproduce `.github/workflows/ci.yml` on native macOS arm64 and in
Ubuntu 24.04 and Debian bookworm amd64 Docker containers. Linux runs use
x86-64 emulation on the Apple Silicon host. The native macOS host uses its
installed SDK rather than claiming to be the GitHub `macos-14` runner.

## Results

| Gate | Result |
| --- | --- |
| macOS portable/OpenMP builds and native MLX/Maple build | PASS |
| macOS `make test` and automatic document/audio/vision routing checks | PASS |
| macOS composer/session UI tests | PASS |
| macOS native PDF OCR and clean bundled OCR installer | PASS |
| Debian `BUILD_DIR=build-debian make debian-portability-test` | PASS, exit 0 |
| Ubuntu portable/OpenMP builds, full `make test`, routing/UI/PDF OCR checks | PASS, exit 0 |

Later test-only fixes were rerun on the affected macOS gateway/UI checks.
The Linux containers received the committed corrections before their
affected tests, with source hashes checked against the host checkout.

Ubuntu's `make test` was resumed at `test-prompt-budget` after its fortified
compiler found an unchecked read result in that fixture. The earlier build,
installer, runtime and memory-harness stages had already passed. The resume
recipe contains every remaining command from the `test` target, followed by
the workflow's routing, UI and OCR gates; it does not skip a remaining test.

## Corrections found by the gates

- Include `samosa_prompt_budget.h` in release packaging and installation;
  the clean installer test verifies that it is staged.
- Make Jobs control flow and the invalid UTF-8 fixture unambiguous to GCC,
  preserving strict compiler warnings.
- Replace the copied Chutni executable atomically. On macOS, overwriting
  the existing executable inode produced a killed process even though its
  bytes and on-disk signature matched the runnable original.
- Install `procps` explicitly in minimal Linux CI containers.
- Keep composer performance and escaping checks aligned with the current
  welcome copy.
- Verify exact exclusion counts and valid representatives without assuming
  filesystem enumeration order.
- Verify actual PDF extraction when PDFium is required, and an explicit
  unavailable-reader result in the minimal Debian portable build.
- Re-fetch scope publication after observing job completion, and require
  the expected evidence generation, avoiding a stale HTTP snapshot.
- Handle the lifetime-pipe read result and interrupted reads in the prompt
  budget fixture, satisfying Ubuntu's fortified compiler checks.

## Isolation and reproducibility

All workflow fixtures use generated documents, isolated application homes,
test backends, and isolated ports. Downloads, its registered memory, existing
user conversations, and application runtime logs were not opened or queried.
These offline CI gates do not close FW-6 browser/real-model acceptance.

The SDK is PDFium chromium/7961, verified against the SHA-256 pins already in
the workflow. NumPy, Pillow and ReportLab are test dependencies; no Python
dependency was added to the product runtime.

Full local logs are retained in `/private/tmp/samosa-local-ci-20261004/`:

- `macos-final.log`, `macos-finish.log`, `macos-publication.log`, and
  `macos-prompt-fixture.log`
- `debian-publication.log`
- `ubuntu-rerun.log` and `ubuntu-resume.log`

The Docker reruns reuse dependency images from the initial clean container
setup. Only the repository and this run's CI directory are mounted. The work
copy excludes model directories and previous build artifacts.

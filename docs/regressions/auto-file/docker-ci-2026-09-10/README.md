# Local Docker CI verification — 2026-09-10/11

The audio/document milestone must pass local Linux Docker gates before the
next push. These checks exercise compiled code, HTTP integrations, packaging,
and installation with fixture backends; they do not qualify real-model Linux
inference or performance.

## Commands and scope

```sh
make ci-debian
make ci-ubuntu-full
```

Both targets use amd64 containers under QEMU on the reference Apple Silicon
Mac, mount the checkout read-only, copy source into a fresh `/work`, and run
builds/tests as the unprivileged `ci` user. Host build directories, `.venv`,
model directories, and Git metadata are excluded from the copy.

- Debian: `debian:bookworm-slim`, Debian 12, GCC 12.2, glibc 2.36;
  `BUILD_DIR=build-debian make debian-portability-test`.
- Ubuntu: `ubuntu:24.04`, Ubuntu 24.04.4, GCC 13.3, glibc 2.39;
  portable and OpenMP builds, full `make test`, document harness, audio
  attachments, Molmo gateway/processor, and composer/session-token UI checks.

The Ubuntu target now includes the new file-routing tests from GitHub CI.
The container image is pinned to the same Ubuntu release family as the
GitHub runner. Exact toolchain and kernel records accompany this report.

## Corrections exercised

- Disabled unused miniz stdio APIs; DOCX uses bounded in-memory ZIP input.
- Checked writes in the extractor's signal-safe timeout handler so fortified
  Linux builds retain `-Werror` without rejecting an ignored return value.
- Checked `fscanf` in the descendant-cancellation test; an unreadable PID
  fails the test rather than allowing a false pass.
- Split the Molmo video processor's conditional error assignment and return
  onto separate lines to satisfy GCC's misleading-indentation check.
- Made the document harness use the requested build directory.
- Replaced obsolete `usleep` in the multimodal cancellation test with POSIX
  `nanosleep`, retaining the delay and handling interrupted sleeps.

## Results

- Debian portability gate: **PASS**, exit 0.
- Ubuntu full gate after the portable sleep correction: **PASS**, exit 0.
  The default Ubuntu archive endpoint timed out from the reference host, so
  the successful run used `CI_UBUNTU_APT_MIRROR=http://mirrors.edge.kernel.org/ubuntu/`.
  APT signature verification and every build/test command remained enabled.
- First Ubuntu full gate: **FAIL**, exit 2, at the multimodal supervisor
  test compilation (`usleep` is not declared with POSIX.1-2008 on glibc).
  Its complete log is retained because it records the failure that prompted
  the portable sleep correction.
- Native macOS multimodal supervisor with the portable sleep: **PASS**.
- Separate Ubuntu document-reader cancellation contract: **PASS**.
- Separate Ubuntu Molmo processor regression: **PASS**.

The final Ubuntu run used a fresh source snapshot containing all corrections.
The final Debian run used the same host source after Ubuntu completed. The
source hashes below include the portable sleep test added after the first
Ubuntu run.

## Evidence

- [First Ubuntu complete log (failed)](ubuntu-first-run.log.gz)
- [Earlier complete Debian pass log](debian.log.gz)
- [Debian environment](debian-environment.txt)
- [Ubuntu environment](ubuntu-environment.txt)
- [Ubuntu source SHA-256 values](source-sha256.txt)

The successful Ubuntu run's terminal was interrupted while Docker continued,
so its live file contains only the package-setup prefix and is not included as
a complete log. The container completion was observed with `docker wait` and
returned exit 0. Compressed logs can be read with `gzip -dc <file>.log.gz`.
Existing format-truncation warnings remain governed by the repository's Linux
warning policy; no warning suppression was added for the defects above.

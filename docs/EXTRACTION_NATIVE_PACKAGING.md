# EX-REL-02: preserve release assembly without Python

Assignment opened 2026-11-20; supports C03/C13/C14 and the user's project-wide
Python-free requirement. The active visual protocol extraction needs clean
installation coverage, whose existing entry points currently execute Python.

Reuse `tools/package_hf.py`'s existing behavior in `tools/package_hf.sh`: same
arguments/defaults, source/runtime selection, model hard-link with copy fallback,
native runtime/model checks, PDFium archive completeness, version/repository
substitution, browser assets and sorted SHA256/size manifests. Port
`tests/test_package_pdfium.py`'s three existing acceptance scenarios to shell and
update the existing shell installer tests to call the replacement. Preserve
runtime-only absence of optional chat-model weights and existing installation
assertions. No new model, installer protocol or runtime design is in scope.

Use this Mac and native shell tools only. Do not invoke Python, start Docker or
run Linux verification. Retire the replaced Python wrapper/test after their
shell equivalents pass. Other Python-dependent tests remain explicitly open.

Acceptance commands from the parent root:

```sh
sh tests/test_package_pdfium.sh
sh tests/test_gateway_installer.sh
sh tests/test_install_path.sh
sh tests/test_atomic_install.sh
```

The runtime-only test's Python directory server is replaced by an optional
configured-root mode in the existing native `fake_model_download_server`. Its
original one-artifact failure modes stay intact; its deterministic test bytes
now come from shell/awk. Run that existing server regression before the
runtime-only install/upgrade test. Record exact commands/results and source identity
here and in the architecture Resume checkpoint. This assignment does not close
full release qualification or visual readiness.

## Result — 2026-11-20

The original three package-selection/version cases pass in shell, with exact
owner source bytes/digests and every emitted manifest size/digest checked.
Clean gateway installation, atomic upgrade/corruption refusal and all six PATH
cases pass on this Mac. The runtime-only installation/upgrade test now uses an
optional directory mode of the existing native download fixture, and passes
with its model-request/legacy-identity assertions unchanged.

The native HTTP fixture's existing failure-mode regression and runtime-only test
pass under `tools/without_python.sh`, which refuses and records PATH-based Python
calls even if a child suppresses their exit status. The guard itself was checked
for propagation of nonzero exit codes and detection of suppressed interpreter
stub calls. It does not replace source review for absolute interpreter paths.
One exploratory fixture invocation executed a second overlooked Python generator;
it is excluded. Both generators now use the same arithmetic byte sequence via
shell/awk, and the guarded rerun passes.

Retired: `tools/package_hf.py` and `tests/test_package_pdfium.py`. Current shell
callers use `tools/package_hf.sh`. The still-legacy real OCR Python test's package
command was redirected to the shell replacement to avoid a stale deleted-file
reference; that driver was not invoked and its native migration remains open.
The original default CI and other Python-backed tests also remain open.

## LIFE-07t native Qwen activation

Versioned native installs now keep `primary-qwen-pins.tsv` beside the release
catalog. `dist/install.sh` uses the catalog digest and size from the release
manifest it already verified. It records the digest and size of the newly built
Qwen executable and, when linked, its local OpenMP library. `dist/samosa`
resolves the versioned runtime path before starting the Gateway.

For catalog Qwen, the Gateway checks the exact catalog bytes against the release
pin and reuses the catalog validator and model artifact path resolver. The six
expected model digests come from the same catalog the existing app download
flow uses. Inference owns the checksum, file identity, loader and generation
qualification. Missing or modified release metadata prevents Qwen startup.
Runtime-only installs still make zero model requests and leave existing models
in place. The verified defaults apply to catalog Qwen. Custom Qwen snapshots
require explicit manual pins, and Maple and llama keep their existing ordinary
launch path pending qualification.

These checks ran on arm64 macOS 26.5.1 with Inference `69e4c65f3de5fd22817f6adca01cf7b133b837cb`.

```sh
./tools/without_python.sh make samosa-gateway
./tools/without_python.sh node tests/test_primary_gateway.mjs
./tools/without_python.sh node tests/test_primary_gateway.mjs --installed
./tools/without_python.sh make test-prompt-budget
./tools/without_python.sh sh tests/test_model_selection.sh
SAMOSA_TEST_REAL_QWEN_MODEL=/private/tmp/samosa-life07h-real.2wwUok/model \
  ./tools/without_python.sh sh tests/test_runtime_only_release.sh
```

The installed fixture exercises Qwen, Bonsai selection, Qwen reselection, a
modified catalog and an absent release pin file. The manual fixture exercises
failed generation rollback, stale input refusal and shutdown. The original
prompt-budget and model-selection suites pass. The runtime-only test validates
pin bytes and checksums on both the initial install and an upgrade and preserves
the existing model fixture's inode, modification time and checksum.
The upgraded launcher qualified the real six-file Qwen bundle, answered a
four-token public chat request and reaped its worker. The memory guard measured
3,632 MB peak process-tree RSS and zero swap growth. The run used isolated
state and APFS clones of the retained model weights.

The original catalog acceptance driver also passed after its JSON assertions
were moved to Node. `./tools/without_python.sh sh tests/test_model_catalog.sh`
checks bundled model facts, optional companion artifacts, platform compatibility,
installed bytes, shared-runtime availability and malformed catalog refusal.

## LIFE-07u native image reclaim

The 16 GiB arm64 Mac ran the upgraded LIFE-07t release built from the parent
source at `60a34f20464a6af1d2f520254e983d701c810daa`. The fixture uses the original
400×400 `assets/samosa-chat.png`, catalog Qwen and the retained native Molmo2 Q4
engine and model. The App state and artifacts were isolated in temporary
directories. Qwen ran with a 2,048-token context and two OpenMP threads.

```sh
visual_root=/private/tmp/samosa-life07u-visual.U6agAX
SAMOSA_TEST_REAL_MOLMO2_ENGINE="$visual_root/runtime/samosa-molmo2" \
SAMOSA_TEST_REAL_MOLMO2_MODEL="$visual_root/molmo2-4b-mlx-q4-v1" \
MAX_FOOTPRINT_MB=6144 MAX_SWAP_DELTA_MB=0 \
  ./tools/without_python.sh bash tools/run_memory_guarded.sh \
  node tests/test_primary_gateway.mjs --installed-release "$(cat "$visual_root/release-path")"
```

The attachment API accepted the PNG. The developer trace recorded Molmo2 ready,
image evidence and specialist shutdown. The chat API returned Qwen's completion.
The test checked the original Qwen PID and the Molmo2 PID had exited, Qwen
remained selected, and a different qualified Qwen PID handled the next request
and was reaped on Gateway shutdown. The process tree peaked at 4,240 MB RSS
with zero swap growth. Both existing Gateway fixture modes also passed.

The first probe used a 210-second test client deadline and verified Molmo2
evidence and Qwen resume before the client stopped waiting for the answer. The
passing run uses a longer test deadline and the original app prefill and read
limits. This result covers the standard image handoff on this host. Wider image
and video routes, VisionPsy fallback and common visual control qualification
remain open.

| Visual input | SHA-256 |
| --- | --- |
| `samosa-molmo2` | `4ebfbb7d38f0955deb2b17dd70361ee4e46ee2b56ab2b1943ca5ff9ccd30bd50` |
| `mlx.metallib` | `846add228d562bb224b11f18c4655b75a05f3336e543654ce49943577e26cd5a` |
| Molmo2 `manifest.json` | `a07230dd952e97969921223328e7812020bb58711728450cdfb2205e3a15b770` |
| `assets/samosa-chat.png` | `5afc7cbbf619e614110507fb06b4d38f62b3ab70103c861d6145653c498da6b1` |

Exact commands, terminal logs, source hashes and the related visual ownership
move are recorded in
[LIFE-07b qualification](../components/samosa-inference/docs/qualification/life07b-specialist-protocol/README.md).
This closes this bounded native packaging replacement; it is not whole-project
Python-free or complete-release qualification. No Docker/Linux execution occurred.

Dependency limit: these assembly checks used the pre-existing modified Chutni
worktree at base f6eeb53, as the original gateway packaging did. Its four modified
files still match the pre-work recovery snapshot byte-for-byte; their exact
hashes are in EXTRACTION_NATIVE_PACKAGING_DEPENDENCIES.sha256. They were not
changed or committed by this assignment. Complete assembled release reproducibility
still requires a qualified clean dependency revision; the passing slice above
must not be presented as that full release gate.

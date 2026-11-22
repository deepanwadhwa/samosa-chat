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

## LIFE-07v native Ornith readiness

The real Ornith backend originally used all four readiness tokens on
`reasoning_content`, leaving the final `content` empty. The same native engine
and model answered the existing Gateway request when it supplied
`chat_template_kwargs.enable_thinking=false`. The Inference probe now reuses
that control for llama-server, and the failure fixture checks that a
reasoning-only response still triggers rollback. The engine and model pins
and exact loader profile are recorded in the component evidence.
[Static engine qualification](../components/samosa-inference/docs/static-engine-package.md#life-07v-gateway-primary-qualification-on-macos).

```sh
./tools/without_python.sh make -C components/samosa-inference test-primary-host sanitize-primary-host
./tools/without_python.sh make samosa-gateway
ornith_root=/private/tmp/samosa-life07v-ornith.Db9hec
SAMOSA_TEST_REAL_ORNITH_ENGINE="$ornith_root/build/bin/llama-server" \
SAMOSA_TEST_REAL_ORNITH_MODEL="$ornith_root/model/Ornith-1.0-9B-Q4_K_M.gguf" \
MAX_FOOTPRINT_MB=6144 MAX_SWAP_DELTA_MB=0 \
  ./tools/without_python.sh bash tools/run_memory_guarded.sh \
  node tests/test_primary_gateway.mjs --actual-ornith
```

The real worker qualified, answered a public chat request and was reaped with
its PID marker. The process tree peaked at 5,721 MB RSS with zero swap growth.
Both Gateway fixture modes passed. This run covers explicit pins for the
native Ornith profile. Automatic release metadata, actual Maple and Bonsai,
and the optional Bonsai projector still need native qualification.

## LIFE-07w bundled Ornith on macOS 26

The existing release packager accepts the reviewed LIFE-07v executable via
`--ornith-runtime` and takes its notices from the pinned checkout supplied via
`--prism-source`. The release manifest covers the executable, source provenance,
minimum OS, and the Prism, jsonhpp and cpp-httplib notices. The installer uses its
existing checksum-verified staging path and publishes the catalog and executable
pins together with the versioned release. Ornith uses that executable on macOS 26
and later. Its GGUF still comes from the existing catalog. Bonsai and explicit
llama-server overrides keep their existing routing.

```sh
ornith_root=/private/tmp/samosa-life07v-ornith.Db9hec
SAMOSA_TEST_REAL_ORNITH_ENGINE="$ornith_root/build/bin/llama-server" \
SAMOSA_TEST_REAL_ORNITH_MODEL="$ornith_root/model/Ornith-1.0-9B-Q4_K_M.gguf" \
SAMOSA_TEST_PRISM_SOURCE="$HOME/.samosa/backends/prism-llama.cpp" \
SAMOSA_TEST_REAL_QWEN_MODEL=/private/tmp/samosa-life07h-real.2wwUok/model \
  ./tools/without_python.sh sh tests/test_runtime_only_release.sh
```

The clean install and upgrade checked the installed executable and pins against
the packaged bytes, installed the notices, and made zero optional chat-model
requests. The upgraded launcher qualified real Qwen and Ornith in separate runs
using APFS-cloned model data and isolated app state. Ornith used the normal
8,192-token auto context and reported ready in the catalog, returned a public
chat answer and reaped its worker. Peak process-tree RSS was 3,553 MB for Qwen
and 5,781 MB for Ornith, with zero swap growth in both runs. The profile was
tested on arm64 macOS 26.5.1.

## LIFE-07x Python-free model manager checks

The existing install and setup drivers generate the same deterministic bytes
with shell `awk` and read JSON with the Node helper already used by selection.
`./tools/without_python.sh make test-model-manager` passed on this Mac. It
exercised the original catalog and UI fixtures, native model install success and
failure, verified replacement, pause, cancel and resume, selection and its
rollback cases, and onboarding recovery across a Gateway restart.

## LIFE-07y real Maple and Bonsai

The existing catalog URLs and digests supplied all 11 Maple files and both
Bonsai files. Acquisition used `curl` and independent SHA-256 and size checks in
temporary directories. The Maple engine was rebuilt through the original
delegated native target. Bonsai used the exact static LIFE-07v engine.
[Maple source and runtime evidence](../components/samosa-inference/docs/qualification/life07i-maple-sources/README.md#life-07y-real-catalog-model-through-the-inference-primary-owner).
[Bonsai native engine evidence](../components/samosa-inference/docs/static-engine-package.md#life-07y-bonsai-text-and-vision-through-the-inference-primary-owner).

```sh
./tools/without_python.sh make samosa-maple samosa-gateway
./tools/without_python.sh make -C components/samosa-inference test-primary-host sanitize-primary-host
model_root=/private/tmp/samosa-life07y-models.4BZk12
SAMOSA_TEST_REAL_MAPLE_ENGINE="$model_root/runtime/samosa-maple" \
SAMOSA_TEST_REAL_MAPLE_MODEL="$model_root/maple" \
MAX_FOOTPRINT_MB=6144 MAX_SWAP_DELTA_MB=0 \
  ./tools/without_python.sh bash tools/run_memory_guarded.sh node tests/test_primary_gateway.mjs --actual-maple
SAMOSA_TEST_REAL_BONSAI_ENGINE=/private/tmp/samosa-life07v-ornith.Db9hec/build/bin/llama-server \
SAMOSA_TEST_REAL_BONSAI_MODEL="$model_root/bonsai" \
MAX_FOOTPRINT_MB=7168 MAX_SWAP_DELTA_MB=0 \
  ./tools/without_python.sh bash tools/run_memory_guarded.sh node tests/test_primary_gateway.mjs --actual-bonsai
```

The four-token Maple probe returned empty visible content, including with the
original template control disabled. The legacy Gateway request and extracted
primary probe both produced final text with 64 tokens. Maple and Bonsai passed
public chat, worker cleanup and marker removal. Bonsai also processed the app
logo through its pinned projector and returned an image answer. Peak process-tree
RSS was 756 MB for Maple and 5,021 MB for Bonsai, with zero swap growth. These
runs use explicit model and runtime pins. Native automatic provisioning is the
next release step.

## LIFE-07z bundled Maple and Bonsai

The installer records Maple's catalog, executable and adjacent Metal library
pins from the verified release manifest. Bonsai and Ornith share the qualified
static llama-server supplied by the existing `--ornith-runtime` packaging flag.
Model pins come from the bundled catalog. The Bonsai projector is verified when
the existing launch configuration includes it. The launcher and `doctor` use
the installed runtime on the qualified Mac. Explicit runtime overrides keep
their existing routing.

The clean install and upgrade fetched no optional chat weights. Native Qwen,
Ornith, Maple, Bonsai text-only and Bonsai with its projector each started via
the normal launcher with automatic pins, answered chat, and reaped their worker.
Bonsai reported text-only without the projector. With it present, the native
engine confirmed it processed the app's image. The catalog, original launcher
fixtures, package-selection and complete model-manager tests passed. The two
Bonsai runs also checked `doctor` through the installed launcher.

| Installed profile | Peak process-tree RSS |
| --- | --- |
| Qwen | 4,037 MB |
| Ornith | 5,945 MB |
| Maple with VisionPsy retry | 1,802 MB |
| Bonsai text-only | 4,668 MB |
| Bonsai with projector | 5,515 MB |

Each guarded run reported zero swap growth on arm64 macOS 26.5.1. The release
staged Inference `5515a10940b72f7c73bce9ed51d72b094cb30d12`.

```sh
model_root=/private/tmp/samosa-life07y-models.4BZk12
ornith_root=/private/tmp/samosa-life07v-ornith.Db9hec
vision_root=/private/tmp/samosa-life08a-native.sICRlv
SAMOSA_TEST_REAL_ORNITH_ENGINE="$ornith_root/build/bin/llama-server" \
SAMOSA_TEST_REAL_ORNITH_MODEL="$ornith_root/model/Ornith-1.0-9B-Q4_K_M.gguf" \
SAMOSA_TEST_PRISM_SOURCE="$HOME/.samosa/backends/prism-llama.cpp" \
SAMOSA_TEST_REAL_QWEN_MODEL=/private/tmp/samosa-life07h-real.2wwUok/model \
SAMOSA_TEST_REAL_MAPLE_ENGINE="$model_root/runtime/samosa-maple" \
SAMOSA_TEST_REAL_MAPLE_MODEL="$model_root/maple" \
SAMOSA_TEST_REAL_BONSAI_MODEL="$model_root/bonsai" \
SAMOSA_TEST_REAL_VISIONPSY_ENGINE="$vision_root/vision-runtime/samosa-visionpsy" \
SAMOSA_TEST_REAL_VISIONPSY_MODEL="$vision_root/visionpsy" \
  ./tools/without_python.sh sh tests/test_runtime_only_release.sh
```

## LIFE-08a native visual retry and long-window speech

Native VisionPsy returned an empty observation and zero generation at the
first tile budget. The low-level protocol retains its wire behavior. The
turn owner recognizes a blank or missing requested observation and uses the
existing one-step lower-side retry. The smaller tile budget produced actual
evidence. The packaged runtime's 1,536-side attempt returned zero characters
and the 1,024-side retry returned a 93-byte observation. Both specialists were
reaped and the same Maple worker answered
using the recovered evidence. A separate fixture verifies a second empty
attempt returns failure and clears the specialist reservation.
[Turn owner evidence](../components/samosa-inference/docs/qualification/life07n-turn-control/README.md#life-08a-visionpsy-empty-observation-recovery).

The original Whisper CLI runner transcribed a 33.482-second synthetic
PCM16 recording with its last phrase beginning at 31.090 seconds. Normal and
ASan/UBSan tests found the phrase in both text and the SRT segment starting
at 30 seconds. They preserved the CLI's existing segmentation and cleaned
both workspaces. Peak process-tree RSS was 356 MB and 375 MB, respectively,
with zero swap growth. The audio SHA-256 was
`010926ca7582e7345a90ed3f24864b2d4e3b70b26cdd2fa15b707b79ca15089f`.
This is a baseline for the long-window owner. The common Whisper and visual
control APIs and shared recovery still need implementation and qualification.
[Whisper process evidence](../components/samosa-inference/docs/qualification/life07l-whisper-guardian/README.md#life-08a-actual-audio-beyond-one-30-second-window).

#### LIFE-08b Whisper request identity

The existing Gateway voice endpoint and audio attachment paths pass the
selected engine and model identity into Inference. Its long-window runner
reuses Activation's file-version comparison to refuse changes detected
before launch or after the guardian has finished. A voice fixture changes
the selected model during execution, receives the existing `voice_not_ready`
error and succeeds on retry. Voice and audio attachment regression tests pass,
including the existing persistent transcript cache assertions.

The authenticated Gateway voice endpoint and the normal and ASan/UBSan
Inference text and SRT drivers transcribed the original 33.482-second WAV
through the checked runner. All runs retained the final phrase and cleaned
their workspaces. The fresh standalone Inference runner and Activation tests
pass under the Python-free guard, as do the exact package source hash checks
and a native runtime-only clean install and upgrade.
[Whisper process evidence](../components/samosa-inference/docs/qualification/life07l-whisper-guardian/README.md#life-08b-input-identity-through-transcript-publication).

#### Earlier packaging qualification

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

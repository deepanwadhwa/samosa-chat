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

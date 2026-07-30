# Samosa Chat — build the engine. `make` for the portable build; `make omp` for
# the multithreaded build.
#
# OpenMP flags are per-platform. Apple's clang does not enable OpenMP itself: it
# needs -Xclang -fopenmp plus Homebrew's libomp (brew install libomp). GCC and
# upstream clang on Linux take a plain -fopenmp and find libgomp/libomp
# themselves — and gcc rejects -Xclang outright, which is why the old
# unconditional flags broke the Linux CI leg. dist/install.sh already branches
# the same way; keep the two in step.
UNAME_S := $(shell uname -s)
BUILD_DIR ?= build
ifeq ($(UNAME_S),Darwin)
  CC ?= clang
  OMP_PREFIX := $(shell [ -d /opt/homebrew/opt/libomp ] && echo /opt/homebrew/opt/libomp || echo /usr/local/opt/libomp)
  OMP_CFLAGS := -Xclang -fopenmp -I$(OMP_PREFIX)/include
  OMP_LDFLAGS := -L$(OMP_PREFIX)/lib -lomp
else
  CC ?= cc
  OMP_CFLAGS := -fopenmp
  OMP_LDFLAGS :=
endif
NUMPY_PYTHON := $(shell python3 -c 'import numpy' >/dev/null 2>&1 && echo python3 || { [ -x .venv/bin/python ] && .venv/bin/python -c 'import numpy' >/dev/null 2>&1 && echo .venv/bin/python; } || { [ -x ../.venv/bin/python ] && echo ../.venv/bin/python; })
ENGINE_HEADERS := $(wildcard src/*.h)
PDFIUM_DIR ?=
PDFIUM_LIBRARY := $(firstword $(wildcard $(PDFIUM_DIR)/lib/libpdfium.*))
CHUTNI_DIR ?= vendor/chutni
CHUTNI_BUILD := $(abspath $(BUILD_DIR)/chutni)
CHUTNI_MCP := $(BUILD_DIR)/chutni-mcp

# PDFium is deliberately optional: the engine's normal build remains
# dependency-free.  The installer supplies a SHA-pinned platform artifact and
# invokes this target with PDFIUM_DIR set to its unpacked root.
ifeq ($(strip $(PDFIUM_DIR)),)
PDFIUM_READY :=
else
PDFIUM_READY := $(PDFIUM_DIR)/include/fpdfview.h $(PDFIUM_LIBRARY)
endif

samosa-engine: src/qwen36b.c src/expert_cache.c src/vision.c $(ENGINE_HEADERS)
	@mkdir -p $(BUILD_DIR)
	$(CC) -O3 -Wno-unused-function -pthread src/qwen36b.c src/expert_cache.c src/vision.c -o $(BUILD_DIR)/qwen36b -lm

samosa-extract: src/samosa_extract.c src/tok.h src/tok_unicode.h src/json.h $(PDFIUM_READY)
	@mkdir -p $(BUILD_DIR)
	@if [ -z "$(PDFIUM_DIR)" ]; then \
	  echo "PDFium support unavailable: set PDFIUM_DIR to an unpacked PDFium artifact" >&2; exit 2; \
	fi
	@if [ -z "$(PDFIUM_LIBRARY)" ]; then \
	  echo "PDFium support unavailable: no libpdfium shared library under $(PDFIUM_DIR)/lib" >&2; exit 2; \
	fi
	$(CC) -O2 -Wall -Wextra -Werror -Wno-unused-function -std=c11 -I$(PDFIUM_DIR)/include \
	  src/samosa_extract.c $(PDFIUM_LIBRARY) \
	  -Wl,-rpath,$(PDFIUM_DIR)/lib -o $(BUILD_DIR)/samosa-extract
	@if [ "$(UNAME_S)" = "Darwin" ]; then \
	  install_name_tool -change ./libpdfium.dylib @rpath/libpdfium.dylib $(BUILD_DIR)/samosa-extract; \
	fi

samosa-fs: src/samosa_fs.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror -std=c11 src/samosa_fs.c -o $(BUILD_DIR)/samosa-fs

# T0.5: build the separate, minimized MIT-licensed Gigatoken adapter. Keeping
# it outside the C gateway means a Rust/toolchain failure cannot make the
# browser control plane unavailable.
gigatoken-adapter:
	sh tools/build_gigatoken_adapter.sh

tok-oracle: tests/tok_oracle.c src/tok.h src/tok_unicode.h src/json.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Wno-unused-function -std=c11 -Isrc tests/tok_oracle.c -o $(BUILD_DIR)/tok-oracle

test-gigatoken-adapter: gigatoken-adapter tok-oracle tests/test_gigatoken_adapter.py
	python3 tests/test_gigatoken_adapter.py

test-gigatoken-supervisor: gigatoken-adapter tests/test_gigatoken_supervisor.c src/samosa_gigatoken.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Wno-unused-function -std=c11 -pthread -Isrc tests/test_gigatoken_supervisor.c -o $(BUILD_DIR)/test_gigatoken_supervisor
	SAMOSA_GIGATOKEN_ADAPTER="$(PWD)/$(BUILD_DIR)/samosa-gigatoken-adapter" SAMOSA_TOKENIZER="$(PWD)/tokenizer_qwen36.json" $(BUILD_DIR)/test_gigatoken_supervisor

samosa-chutni-db: src/samosa_chutni_db.c src/sqlite/sqlite3.c src/sqlite/sqlite3.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror -Wno-unused-function -std=c11 -Isrc -DSQLITE_THREADSAFE=1 -DSQLITE_ENABLE_FTS5 src/samosa_chutni_db.c src/sqlite/sqlite3.c -o $(BUILD_DIR)/samosa-chutni-db -lpthread -ldl -lm

# The generic service is pinned as a submodule and shipped as an application
# runtime component. Samosa calls its public JSON tool surface; it does not
# compile or maintain a private Chutni storage implementation.
.PHONY: chutni-service
chutni-service:
	@test -f "$(CHUTNI_DIR)/Makefile" || { echo "missing Chutni submodule; run: git submodule update --init" >&2; exit 2; }
	$(MAKE) -C "$(CHUTNI_DIR)" BUILD="$(CHUTNI_BUILD)" "$(CHUTNI_BUILD)/chutni-mcp"
	@mkdir -p $(BUILD_DIR)
	cp "$(CHUTNI_BUILD)/chutni-mcp" "$(CHUTNI_MCP)"

test-chutni-db: samosa-chutni-db tests/test_chutni_db.sh tests/test_chutni_scope.sh tests/test_chutni_extraction_cache.sh tests/test_chutni_recovery.sh
	SAMOSA_CHUTNI_DB="$(PWD)/$(BUILD_DIR)/samosa-chutni-db" sh tests/test_chutni_db.sh
	SAMOSA_CHUTNI_DB="$(PWD)/$(BUILD_DIR)/samosa-chutni-db" sh tests/test_chutni_scope.sh
	SAMOSA_CHUTNI_DB="$(PWD)/$(BUILD_DIR)/samosa-chutni-db" sh tests/test_chutni_extraction_cache.sh
# T4.5 injected-failure recovery. Uses a real RAM disk for ENOSPC on macOS and
# reports SKIP elsewhere rather than pretending the case ran.
	SAMOSA_CHUTNI_DB="$(PWD)/$(BUILD_DIR)/samosa-chutni-db" sh tests/test_chutni_recovery.sh

# Kimi Linear metadata preflight. This does not download or quantize the model;
# the pure-C KDA/MLA runtime must land before a weight converter is enabled.
test-kimi-converter: tools/convert_kimi_linear.py tests/test_kimi_conversion_plan.py
	python3 tests/test_kimi_conversion_plan.py

# test-chutni: T0.2+ gate for docs/TASKS_UI_CHUTNI.md Chutni core work
# (streaming inventory scan, full-hash-on-request, symlink/permission safety).
test-chutni: samosa-fs tests/test_chutni_inventory.sh
	SAMOSA_FS="$$PWD/$(BUILD_DIR)/samosa-fs" sh tests/test_chutni_inventory.sh

# test-model-manager: Phase 2 gate for docs/TASKS_UI_CHUTNI.md (section 5.3,
# the model catalog and lifecycle). T2.1: GET /v1/models/catalog serves the
# real bundled assets/models.json (real, independently-verified Qwen/
# Bonsai/Ornith artifact bytes and SHA-256 hashes), validates it before
# trusting any of it, and layers live compatible/install_state/active
# detection on top using the gateway's existing per-backend fields. The
# frontend half of T2.1 (assets/app.html's model <select>, rebuilt from this
# endpoint instead of a hardcoded option list) has its own DOM-fixture
# coverage in tests/test_model_catalog_ui.mjs, same pattern as
# tests/test_chooser_ui.mjs. T2.2: POST /v1/models/install and friends
# implement real resumable, verified, atomically-activated downloads against
# tests/fake_model_download_server.c (T0.1), covering every documented
# failure mode plus pause/resume/cancel/retry and the single-active-transfer
# gate. T2.3: POST /v1/backends/select now waits for real readiness and a
# weights-file fingerprint before a switch reports success, rolling back to
# the prior working backend on a readiness timeout, an immediate child
# crash, a fingerprint mismatch, or a durable-registry commit failure --
# tests/test_model_selection.sh against tests/fake_openai_backend.c (T0.1's
# /healthz alias and SAMOSA_FAKE_HEALTH_DELAY_MS knob). T2.4:
# tests/test_setup_flow.sh covers GET /v1/setup/status's next_step now
# resolving against this same real catalog/install/selection state (rather
# than the old T1.2/T1.4 interim bridge), selection persistence at
# install-start and backend-select time, legacy-install adoption, and
# install-job repair across a gateway restart. The frontend half (the real
# Name/Welcome/Model setup screens in assets/app.html, replacing the
# hardcoded-nothing that existed before) has its own DOM-fixture coverage in
# tests/test_setup_flow_ui.mjs, same pattern as tests/test_model_catalog_ui.mjs.
test-model-manager: samosa-gateway test_fake_openai_backend fake_model_download_server tests/test_model_catalog.sh tests/test_model_install.sh tests/test_model_catalog_ui.mjs tests/test_model_selection.sh tests/test_setup_flow.sh tests/test_setup_flow_ui.mjs
	sh tests/test_model_catalog.sh
	sh tests/test_model_install.sh
	node tests/test_model_catalog_ui.mjs
	sh tests/test_model_selection.sh
	sh tests/test_setup_flow.sh
	node tests/test_setup_flow_ui.mjs

# samosa-ocr: the reader sidecar (R2/R3). Portable build; the OMP build is ~2.5x
# faster on a first read (reads are cached forever after). stb_image is compiled
# with -Wno-unused-function like the engine.
samosa-ocr: src/samosa_ocr.c src/kernels.h src/json.h src/stb_image.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -O3 -Wno-unused-function -std=c11 -Isrc src/samosa_ocr.c -o $(BUILD_DIR)/samosa-ocr -lm

samosa-ocr-omp: src/samosa_ocr.c src/kernels.h src/json.h src/stb_image.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -O3 -Wno-unused-function -pthread $(OMP_CFLAGS) -std=c11 -Isrc \
	  src/samosa_ocr.c $(OMP_LDFLAGS) -o $(BUILD_DIR)/samosa-ocr-omp -lm

# ocr-test: offline gate. Validates the C forward pass numerically against the
# NumPy golden tensors (tools/testdata/ocr) that E-R1 verified against PaddleOCR.
ocr-test: samosa-ocr tests/test_samosa_ocr.sh tools/testdata/ocr/det.gold
	SAMOSA_OCR="$$PWD/$(BUILD_DIR)/samosa-ocr" sh tests/test_samosa_ocr.sh

# read-cache-test: offline gate for the content-addressed doc.read cache (R4,
# plus T0.3's locking/fsync/pruning correctness work).
read-cache-test: tests/test_read_cache.c src/read_cache.h src/json.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Wno-unused-function -std=c11 -pthread tests/test_read_cache.c -o $(BUILD_DIR)/test_read_cache
	$(BUILD_DIR)/test_read_cache

# durable-job-test: T0.4 (docs/TASKS_UI_CHUTNI.md) offline gate for the
# generalized durable background-operation primitive (path-jailed job dirs,
# atomic state, §5.7-shaped sequenced events, and verifiable-identity
# one-writer locks) that T2.2 model downloads and T4.x Chutni builds will
# both build on.
durable-job-test: tests/test_durable_job.c src/durable_job.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Wno-unused-function -std=c11 tests/test_durable_job.c -o $(BUILD_DIR)/test_durable_job
	$(BUILD_DIR)/test_durable_job

# doc-read-test: offline gate for doc.read tool handler and cascade (R4).
doc-read-test: samosa-gateway samosa-ocr test_fake_openai_backend tests/test_doc_read.sh
	sh tests/test_doc_read.sh

# motto-test: offline gate for E-R3 20-file motto scenario + cache + review_required parking.
motto-test: samosa-gateway samosa-ocr test_fake_openai_backend tests/test_motto_scenario.sh
	sh tests/test_motto_scenario.sh

# tier2-test: offline gate for R5 Tier-2 Bonsai crop escalation.
tier2-test: samosa-gateway samosa-ocr test_fake_openai_backend tests/test_tier2_escalation.sh
	sh tests/test_tier2_escalation.sh

# r7-r6-test: offline gate for R7 classifier and R6 rec_hand handwriting recognizer head.
r7-r6-test: samosa-gateway samosa-ocr test_fake_openai_backend tests/test_r7_r6_handwriting.sh
	sh tests/test_r7_r6_handwriting.sh

samosa-gateway: src/samosa_gateway.c src/samosa_http.h src/json.h chutni-service
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror -Wno-unused-function -std=c11 -pthread -Isrc \
	  src/samosa_gateway.c -o $(BUILD_DIR)/samosa-gateway

# The HTTP controller invokes the same generic service that MCP hosts use.
chutni-gateway-test: samosa-gateway chutni-service test_fake_openai_backend tests/test_chutni_gateway.sh tests/test_chutni_controls.sh
	sh tests/test_chutni_gateway.sh
	sh tests/test_chutni_controls.sh

detached-service-test: samosa-gateway chutni-service test_fake_openai_backend tests/test_samosa_detached_service.sh
	sh tests/test_samosa_detached_service.sh

# samosa-jobsd is the same source under a launchd-friendly name. Invoked as
# `samosa-jobsd jobsd-once` it polls armed schedules and exits — no listener,
# no backend — which is exactly what the installed launchd plist fires.
samosa-jobsd: src/samosa_gateway.c src/samosa_http.h src/json.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror -Wno-unused-function -std=c11 -pthread -Isrc \
	  src/samosa_gateway.c -o $(BUILD_DIR)/samosa-jobsd

test_fake_openai_backend: tests/fake_openai_backend.c src/samosa_http.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror -Wno-unused-function -std=c11 -pthread -Isrc \
	  tests/fake_openai_backend.c -o $(BUILD_DIR)/test_fake_openai_backend

# fake_model_download_server: deterministic stand-in for the trusted model
# catalog's artifact host (docs/TASKS_UI_CHUTNI.md T0.1/T2.2). Ordinary tests
# must never fetch a real multi-gigabyte model artifact.
fake_model_download_server: tests/fake_model_download_server.c src/samosa_http.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror -Wno-unused-function -std=c11 -pthread -Isrc \
	  tests/fake_model_download_server.c -o $(BUILD_DIR)/fake_model_download_server

test-fake-download-server: fake_model_download_server tests/test_fake_model_download_server.sh
	sh tests/test_fake_model_download_server.sh

# test-ui-setup: Phase 0/1 gate for docs/TASKS_UI_CHUTNI.md. Validates the
# frozen v1 JSON contract fixtures (T0.1), the shared Chutni fixture
# generators (T0.1), the zero-model control-plane startup contract
# (T1.1: the gateway binds and serves setup/health/diagnostics -- and Chat
# fails closed with 409 model_required -- with no model installed anywhere),
# profile/setup state (T1.2), the safe browser directory chooser (T1.3),
# conversation schema v2 migration + gateway-enforced model binding (T1.4),
# and the fail-closed-by-default /v1/ dispatcher gate (any new v1 route not
# on the closed legacy-exemption list requires a valid session token before
# route matching, so it can't ship unauthenticated by omission).
test-ui-setup: test-fake-download-server test_fake_openai_backend samosa-gateway tests/test_chutni_folder_fixture.sh tests/test_ui_chutni_contracts.py tests/test_zero_model_startup.sh tests/test_profile_setup.sh tests/test_fs_chooser.sh tests/test_chooser_ui.mjs tests/test_conversation_binding.sh tests/test_conversation_migration_ui.mjs tests/test_v1_fail_closed_default.sh tests/test_composer_ui.mjs tests/test_composer_perf.mjs
	sh tests/test_chutni_folder_fixture.sh
	python3 tests/test_ui_chutni_contracts.py
	sh tests/test_zero_model_startup.sh
	sh tests/test_profile_setup.sh
	sh tests/test_fs_chooser.sh
	node tests/test_chooser_ui.mjs
	sh tests/test_conversation_binding.sh
	node tests/test_conversation_migration_ui.mjs
	sh tests/test_v1_fail_closed_default.sh
	node tests/test_composer_ui.mjs
	node tests/test_composer_perf.mjs

compiled-gateway-test: samosa-gateway samosa-jobsd samosa-fs test_fake_openai_backend tests/test_compiled_gateway.sh tests/test_settings_compact_proxy.sh tests/test_attachments.sh tests/test_web_search.sh
	SAMOSA_COMPILED_GATEWAY="$$PWD/$(BUILD_DIR)/samosa-gateway" \
	SAMOSA_COMPILED_JOBSD="$$PWD/$(BUILD_DIR)/samosa-jobsd" \
	SAMOSA_FAKE_BACKEND="$$PWD/$(BUILD_DIR)/test_fake_openai_backend" \
	SAMOSA_FS="$$PWD/$(BUILD_DIR)/samosa-fs" sh tests/test_compiled_gateway.sh
	sh tests/test_settings_compact_proxy.sh
	SAMOSA_EXTRACT="$${SAMOSA_EXTRACT:-$$PWD/$(BUILD_DIR)/samosa-extract}" \
	SAMOSA_OCR="$${SAMOSA_OCR:-$$PWD/$(BUILD_DIR)/samosa-ocr}" \
	sh tests/test_attachments.sh
	SAMOSA_COMPILED_GATEWAY="$$PWD/$(BUILD_DIR)/samosa-gateway" \
	SAMOSA_FAKE_BACKEND="$$PWD/$(BUILD_DIR)/test_fake_openai_backend" \
	sh tests/test_web_search.sh

# doc-read-pdf-paging-test: T0.3 (docs/TASKS_UI_CHUTNI.md) real-extractor
# regression for the PDF page-batch-cap fix. Skips gracefully (exit 0) if no
# real samosa-extract is available -- it deliberately does NOT depend on the
# samosa-extract Makefile target, which hard-fails without PDFIUM_DIR.
doc-read-pdf-paging-test: samosa-gateway samosa-fs test_fake_openai_backend tests/test_doc_read_pdf_paging.sh
	SAMOSA_EXTRACT="$${SAMOSA_EXTRACT:-$$PWD/$(BUILD_DIR)/samosa-extract}" \
	SAMOSA_FS="$$PWD/$(BUILD_DIR)/samosa-fs" \
	SAMOSA_FAKE_BACKEND="$$PWD/$(BUILD_DIR)/test_fake_openai_backend" \
	sh tests/test_doc_read_pdf_paging.sh

extract-test: samosa-extract tests/test_samosa_extract.sh tests/fixtures/documents/hello.pdf
	SAMOSA_EXTRACT=./$(BUILD_DIR)/samosa-extract sh tests/test_samosa_extract.sh

extract-tokenizer-test: samosa-extract tests/test_samosa_extract.sh
	@test -n "$(SAMOSA_EXTRACT_TOKENIZER)" || { echo "set SAMOSA_EXTRACT_TOKENIZER to run exact-token tests" >&2; exit 2; }
	SAMOSA_EXTRACT=./$(BUILD_DIR)/samosa-extract SAMOSA_EXTRACT_TOKENIZER="$(SAMOSA_EXTRACT_TOKENIZER)" sh tests/test_samosa_extract.sh

document-installer-test: tests/test_document_installer.sh
	sh tests/test_document_installer.sh

omp: src/qwen36b.c src/expert_cache.c src/vision.c $(ENGINE_HEADERS)
	@mkdir -p $(BUILD_DIR)
	$(CC) -O3 -Wno-unused-function -pthread $(OMP_CFLAGS) \
	  src/qwen36b.c src/expert_cache.c src/vision.c -o $(BUILD_DIR)/qwen36b -lm $(OMP_LDFLAGS)

install: omp samosa-gateway samosa-jobsd samosa-fs samosa-ocr chutni-service
	sh tools/install_local_dev.sh

# E-X5 experiment build only — never shipped. Same as `omp` plus
# -DSAMOSA_SCHED_RUNTIME, so OMP_SCHEDULE picks the hot-kernel schedule at run
# time. Separate output name so it can never be installed by mistake.
omp-sched-runtime: src/qwen36b.c src/expert_cache.c src/vision.c $(ENGINE_HEADERS)
	@mkdir -p $(BUILD_DIR)
	$(CC) -O3 -Wno-unused-function -pthread $(OMP_CFLAGS) -DSAMOSA_SCHED_RUNTIME \
	  src/qwen36b.c src/expert_cache.c src/vision.c -o $(BUILD_DIR)/qwen36b-sched-runtime -lm $(OMP_LDFLAGS)

# E-X10 M0 experiment only — never linked into or installed as qwen36b.
# This target is intentionally Darwin-only: it exercises Apple's Metal API
# and compares the shader with the exact NEON/OpenMP grouped-q4 reference.
metal-spike: tools/metal_spike.m src/kernels.h
	@mkdir -p $(BUILD_DIR)
	@if [ "$(UNAME_S)" != "Darwin" ]; then \
	  echo "metal-spike requires macOS and Apple Metal" >&2; exit 2; \
	fi
	$(CC) -O3 -Wall -Wextra -Wno-unused-function -Wno-unknown-pragmas \
	  -fobjc-arc -pthread $(OMP_CFLAGS) -Isrc tools/metal_spike.m \
	  -o $(BUILD_DIR)/metal-spike -framework Foundation -framework Metal -lm $(OMP_LDFLAGS)

# E-X10 M1 system experiment — separate binary, opt-in again at runtime with
# SAMOSA_METAL=1. It keeps the normal qwen36b and installer CPU-only.
metal-omp: src/qwen36b.c src/expert_cache.c src/vision.c src/metal_expert.m $(ENGINE_HEADERS)
	@mkdir -p $(BUILD_DIR)
	@if [ "$(UNAME_S)" != "Darwin" ]; then \
	  echo "metal-omp requires macOS and Apple Metal" >&2; exit 2; \
	fi
	$(CC) -O3 -Wno-unused-function -Wno-unknown-pragmas -pthread \
	  $(OMP_CFLAGS) -DSAMOSA_METAL -fobjc-arc \
	  src/qwen36b.c src/expert_cache.c src/vision.c src/metal_expert.m \
	  -o $(BUILD_DIR)/qwen36b-metal -framework Foundation -framework Metal -lm $(OMP_LDFLAGS)

pagecache-residency: tools/pagecache_residency.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror -std=c11 tools/pagecache_residency.c -o $(BUILD_DIR)/pagecache-residency

pagecache-residency-test: pagecache-residency tests/test_pagecache_residency.sh
	sh tests/test_pagecache_residency.sh ./$(BUILD_DIR)/pagecache-residency

test: pagecache-residency-test tests/test_expert_cache.c tests/test_kv_cache.c tests/test_repetition_guard.c tests/test_thinking_budget.c tests/test_groupwise_q4.c tests/test_samosa_serve.c tests/test_samosa_wrapper.sh tests/test_atomic_install.sh tests/test_install_path.sh tests/test_gateway_installer.sh tests/test_runtime_only_release.sh tests/test_thinking_output.py tests/test_regression_gate.py tests/test_openrouter_control.py tests/test_route_analysis.py tests/test_spec_accept.py tests/test_converter_quant.py tests/test_package_pdfium.py
	@mkdir -p $(BUILD_DIR)
	$(CC) -O1 -Isrc tests/test_expert_cache.c src/expert_cache.c -o $(BUILD_DIR)/test_expert_cache && ./$(BUILD_DIR)/test_expert_cache
	$(CC) -O1 -Itests tests/test_kv_cache.c tests/kv_cache.c -o $(BUILD_DIR)/test_kv_cache -lm && ./$(BUILD_DIR)/test_kv_cache
	$(CC) -O1 -Isrc tests/test_repetition_guard.c -o $(BUILD_DIR)/test_repetition_guard && ./$(BUILD_DIR)/test_repetition_guard
	$(CC) -O1 -Isrc tests/test_thinking_budget.c -o $(BUILD_DIR)/test_thinking_budget && ./$(BUILD_DIR)/test_thinking_budget
	$(CC) -O1 -Isrc tests/test_groupwise_q4.c -o $(BUILD_DIR)/test_groupwise_q4 -lm && ./$(BUILD_DIR)/test_groupwise_q4
	$(CC) -O1 -pthread -Isrc tests/test_samosa_serve.c src/expert_cache.c src/vision.c -o $(BUILD_DIR)/test_samosa_serve -lm && ./$(BUILD_DIR)/test_samosa_serve
	sh tests/test_samosa_wrapper.sh
	sh tests/test_atomic_install.sh
	sh tests/test_install_path.sh
	sh tests/test_gateway_installer.sh
	sh tests/test_runtime_only_release.sh
	python3 tests/test_thinking_output.py
	python3 tests/test_regression_gate.py
	python3 tests/test_openrouter_control.py
	python3 tests/test_route_analysis.py
	python3 tests/test_spec_accept.py
	python3 tests/test_package_pdfium.py
	@if [ -n "$(NUMPY_PYTHON)" ]; then $(NUMPY_PYTHON) tests/test_converter_quant.py; \
	else echo "converter quant tests: SKIP (NumPy environment unavailable)"; fi
# The compiled gateway, Chutni, and the Kimi preflight are part of the default
# gate. They were previously reachable only by name, so a regression in any of
# them left `make test` green. Run as sub-makes, not prerequisites: several
# bind fixed ports and must not overlap under `make -j`.
	$(MAKE) compiled-gateway-test
	$(MAKE) test-chutni-db
	$(MAKE) test-chutni
	$(MAKE) chutni-gateway-test
	$(MAKE) detached-service-test
	$(MAKE) test-kimi-converter
# T3.3 view logic. Skips with a message where node is unavailable rather than
# passing silently -- the rendering itself cannot be verified headlessly.
	sh tests/test_chutni_views.sh
	sh tests/test_hidden_toggles.sh
# Backend sizing must be correct for machines this repo cannot run on, so the
# tier table is unit-tested and guarded against drifting from the gateway.
	$(CC) -O1 -Wall -Wextra -Werror -std=c11 tests/test_backend_limits.c -o $(BUILD_DIR)/test_backend_limits && ./$(BUILD_DIR)/test_backend_limits
	sh tests/test_backend_limits_match.sh

# test-all adds the gates that need a toolchain beyond a C compiler. The
# Gigatoken adapter needs `cargo +nightly`, so it is deliberately not in
# `make test`: a machine without Rust must still be able to run the full
# offline gate for everything else.
test-all: test
	$(MAKE) test-gigatoken-adapter
	$(MAKE) test-gigatoken-supervisor

# Jobs acceptance (offline). Gate 11 removed the Python jobs modules
# (samosa_jobs/samosa_gateway/samosa_tools/jobs_fs) after native parity, so the
# Jobs runtime under test is the compiled gateway/jobsd/fs. The shipped samosa-fs
# sidecar has direct CLI coverage in tests/jobs/, and every C job route (chat,
# run/find/answer, definition preview/run, move/apply/undo, schedule/jobsd,
# launchd, public-inputs, kill) is exercised by tests/test_compiled_gateway.sh
# with python3 removed from PATH.
jobs-test: samosa-fs
	SAMOSA_FS="$$PWD/$(BUILD_DIR)/samosa-fs" python3 -m unittest discover -s tests/jobs -v
	node tests/test_jobs_ui.mjs
	$(MAKE) compiled-gateway-test

clean:
	rm -rf $(BUILD_DIR)

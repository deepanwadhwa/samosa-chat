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
CXX ?= c++
PRISM_LLAMA_DIR ?= $(HOME)/.samosa/backends/prism-llama.cpp
PRISM_LLAMA_BUILD ?= $(PRISM_LLAMA_DIR)/build
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
ifeq ($(UNAME_S),Darwin)
  DL_LDFLAGS :=
  PDFIUM_LOCAL_RPATH := @loader_path
else
  DL_LDFLAGS := -ldl
  PDFIUM_LOCAL_RPATH := '$$ORIGIN'
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
	$(CC) -O2 $(CWARN) -Wno-unused-function -std=c11 -I$(PDFIUM_DIR)/include \
	  src/samosa_extract.c $(PDFIUM_LIBRARY) \
	  -Wl,-rpath,$(PDFIUM_LOCAL_RPATH) -Wl,-rpath,$(PDFIUM_DIR)/lib \
	  -o $(BUILD_DIR)/samosa-extract
	@if [ "$(UNAME_S)" = "Darwin" ]; then \
	  install_name_tool -change ./libpdfium.dylib @rpath/libpdfium.dylib $(BUILD_DIR)/samosa-extract; \
	fi

samosa-fs: src/samosa_fs.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 $(CWARN) -std=c11 src/samosa_fs.c -o $(BUILD_DIR)/samosa-fs

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
	$(CC) $(CHUTNI_OPT) $(CWARN) -Wno-unused-function -std=c11 -Isrc -DSQLITE_THREADSAFE=1 -DSQLITE_ENABLE_FTS5 src/samosa_chutni_db.c src/sqlite/sqlite3.c -o $(BUILD_DIR)/samosa-chutni-db -lpthread -ldl -lm

ifeq ($(UNAME_S),Darwin)
  CHUTNI_OPT := -O2 -Wno-error=implicit-function-declaration
  CWARN := -Wall -Wextra -Werror
else
  LINUX_WARNING_FLAGS := -Werror -Wno-error=format-truncation -Wno-error=discarded-qualifiers
  CHUTNI_OPT := -O2 -D_GNU_SOURCE -Wno-error=implicit-function-declaration -Wno-error=format-truncation
  CWARN := -Wall -Wextra $(LINUX_WARNING_FLAGS)
endif

# The generic service is pinned as a submodule and shipped as an application
# runtime component. Samosa calls its public JSON tool surface; it does not
# compile or maintain a private Chutni storage implementation.
.PHONY: chutni-service
chutni-service:
	@test -f "$(CHUTNI_DIR)/Makefile" || { echo "missing Chutni submodule; run: git submodule update --init" >&2; exit 2; }
	$(MAKE) -C "$(CHUTNI_DIR)" BUILD="$(CHUTNI_BUILD)" OPT="$(CHUTNI_OPT)" "$(CHUTNI_BUILD)/chutni-mcp"
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
# frontend originally rendered this into a plain model <select> that could
# only switch between already-installed backends; Settings now reuses the
# setup flow's own catalog-driven model cards (Download/Pause/Resume/Cancel/
# Retry/Use) via refreshSettingsModels(), closing the "no way to add a model
# after onboarding" gap without a second, weaker implementation. Its own
# DOM-fixture coverage is in tests/test_model_catalog_ui.mjs, same pattern as
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

# Browser-native, local TTS settings and reply playback. No local test server
# is needed: the DOM fixture supplies the Web Speech API surface.
test-voice-ui: tests/test_voice_ui.mjs assets/app.html
	node tests/test_voice_ui.mjs

# Pure local wake-word feature, DTW, enrollment-validation, and bounded-ring
# fixtures. The test evaluates the exact shipped browser algorithm block.
test-wake-word-ui: tests/test_wake_word_ui.mjs assets/app.html
	node tests/test_wake_word_ui.mjs

# Persistent desktop sidebar collapse, multi-select conversation management,
# and the in-app deletion confirmation (no browser-native confirm box).
test-sidebar-ui: tests/test_sidebar_ui.mjs assets/app.html
	node tests/test_sidebar_ui.mjs

test-kokoro-native: samosa-gateway tests/test_kokoro_native_gateway.sh tests/fake_kokoro_native.c src/samosa_kokoro.h
	sh tests/test_kokoro_native_gateway.sh

# Local hands-free voice: actual token-gated WAV validation and Whisper CLI
# invocation through the compiled gateway, plus the browser UI fixture.
test-voice: samosa-gateway tests/test_voice_gateway.sh test-voice-ui test-wake-word-ui test-kokoro-native
	sh tests/test_voice_gateway.sh

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
	$(CC) -O2 $(CWARN) -Wno-unused-function -std=c11 -pthread -Isrc \
	  src/samosa_gateway.c -o $(BUILD_DIR)/samosa-gateway $(DL_LDFLAGS)

# Falconsai/text_summarization is T5 (encoder-decoder), so it needs the
# dedicated llama_encode/llama_decode helper rather than llama-server's
# decoder-only completion route. Release builds stage this executable and the
# six small shared libraries beside the ordinary Samosa runtime; end users do
# not need a compiler or Python to run summaries.
samosa-summarizer: src/samosa_summarizer.cpp
	@test -f "$(PRISM_LLAMA_DIR)/include/llama.h" || { echo "missing pinned Prism llama.cpp at $(PRISM_LLAMA_DIR)" >&2; exit 2; }
	@test -f "$(PRISM_LLAMA_BUILD)/bin/libllama.0.dylib" || { echo "missing built Prism libraries under $(PRISM_LLAMA_BUILD)/bin" >&2; exit 2; }
	@mkdir -p $(BUILD_DIR)
	$(CXX) -O3 -std=c++17 -I"$(PRISM_LLAMA_DIR)/include" -I"$(PRISM_LLAMA_DIR)/ggml/include" \
	  src/samosa_summarizer.cpp -o $(BUILD_DIR)/samosa-summarizer \
	  -Wl,-rpath,@loader_path/../lib -Wl,-rpath,"$(PRISM_LLAMA_BUILD)/bin" \
	  "$(PRISM_LLAMA_BUILD)/bin/libllama.0.dylib" \
	  "$(PRISM_LLAMA_BUILD)/bin/libggml.0.dylib" \
	  "$(PRISM_LLAMA_BUILD)/bin/libggml-cpu.0.dylib" \
	  "$(PRISM_LLAMA_BUILD)/bin/libggml-blas.0.dylib" \
	  "$(PRISM_LLAMA_BUILD)/bin/libggml-metal.0.dylib" \
	  "$(PRISM_LLAMA_BUILD)/bin/libggml-base.0.dylib"

test-native-summarizer-real: samosa-summarizer tests/test_native_summarizer.c
	@test -n "$(SAMOSA_SUMMARIZER_TEST_MODEL)" || { echo "set SAMOSA_SUMMARIZER_TEST_MODEL to the verified GGUF" >&2; exit 2; }
	$(CC) -O2 $(CWARN) -std=c11 tests/test_native_summarizer.c \
	  -o $(BUILD_DIR)/test_native_summarizer
	$(BUILD_DIR)/test_native_summarizer $(BUILD_DIR)/samosa-summarizer "$(SAMOSA_SUMMARIZER_TEST_MODEL)"

# The HTTP controller invokes the same generic service that MCP hosts use.
chutni-gateway-test: samosa-gateway chutni-service test_fake_openai_backend tests/test_chutni_gateway.sh tests/test_chutni_controls.sh
	sh tests/test_chutni_gateway.sh
	sh tests/test_chutni_controls.sh

detached-service-test: samosa-gateway chutni-service test_fake_openai_backend tests/test_samosa_detached_service.sh
	sh tests/test_samosa_detached_service.sh

app-lifecycle-test: samosa-gateway chutni-service test_fake_openai_backend tests/test_samosa_app_lifecycle.sh tests/test_samosa_llama_lifecycle.sh
	sh tests/test_samosa_app_lifecycle.sh
	sh tests/test_samosa_llama_lifecycle.sh

# samosa-jobsd is the same source under a launchd-friendly name. Invoked as
# `samosa-jobsd jobsd-once` it polls armed schedules and exits — no listener,
# no backend — which is exactly what the installed launchd plist fires.
samosa-jobsd: src/samosa_gateway.c src/samosa_http.h src/json.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 $(CWARN) -Wno-unused-function -std=c11 -pthread -Isrc \
	  src/samosa_gateway.c -o $(BUILD_DIR)/samosa-jobsd

test_fake_openai_backend: tests/fake_openai_backend.c src/samosa_http.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 $(CWARN) -Wno-unused-function -std=c11 -pthread -Isrc \
	  tests/fake_openai_backend.c -o $(BUILD_DIR)/test_fake_openai_backend

test_fake_native_summarizer: tests/fake_native_summarizer.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 $(CWARN) -std=c11 tests/fake_native_summarizer.c \
	  -o $(BUILD_DIR)/test_fake_native_summarizer

test-native-summarizer-supervisor: test_fake_native_summarizer tests/test_native_summarizer_supervisor.c src/samosa_gateway.c
	$(CC) -O1 $(CWARN) -Wno-unused-function -std=c11 -pthread -Isrc \
	  tests/test_native_summarizer_supervisor.c -o $(BUILD_DIR)/test_native_summarizer_supervisor $(DL_LDFLAGS)
	$(BUILD_DIR)/test_native_summarizer_supervisor \
	  $(BUILD_DIR)/test_fake_native_summarizer tests/fixtures/native-summarizer/model.gguf

test-runtime-settings: tests/test_runtime_settings.c src/samosa_gateway.c
	@mkdir -p $(BUILD_DIR)
	$(CC) -O1 $(CWARN) -Wno-unused-function -std=c11 -pthread -Isrc \
	  tests/test_runtime_settings.c -o $(BUILD_DIR)/test_runtime_settings
	$(BUILD_DIR)/test_runtime_settings

# fake_model_download_server: deterministic stand-in for the trusted model
# catalog's artifact host (docs/TASKS_UI_CHUTNI.md T0.1/T2.2). Ordinary tests
# must never fetch a real multi-gigabyte model artifact.
fake_model_download_server: tests/fake_model_download_server.c src/samosa_http.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 $(CWARN) -Wno-unused-function -std=c11 -pthread -Isrc \
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
test-ui-setup: test-fake-download-server test_fake_openai_backend samosa-gateway tests/test_chutni_folder_fixture.sh tests/test_ui_chutni_contracts.py tests/test_zero_model_startup.sh tests/test_profile_setup.sh tests/test_fs_chooser.sh tests/test_chooser_ui.mjs tests/test_conversation_binding.sh tests/test_conversation_migration_ui.mjs tests/test_v1_fail_closed_default.sh tests/test_composer_ui.mjs tests/test_composer_perf.mjs tests/test_web_activity_ui.mjs
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
	node tests/test_web_activity_ui.mjs

compiled-gateway-test: samosa-gateway samosa-jobsd samosa-fs test_fake_openai_backend test_fake_native_summarizer test-native-summarizer-supervisor test-runtime-settings tests/test_compiled_gateway.sh tests/test_settings_compact_proxy.sh tests/test_attachments.sh tests/test_web_search.sh
	BUILD_DIR="$(BUILD_DIR)" \
	SAMOSA_COMPILED_GATEWAY="$$PWD/$(BUILD_DIR)/samosa-gateway" \
	SAMOSA_COMPILED_JOBSD="$$PWD/$(BUILD_DIR)/samosa-jobsd" \
	SAMOSA_FAKE_BACKEND="$$PWD/$(BUILD_DIR)/test_fake_openai_backend" \
	SAMOSA_FS="$$PWD/$(BUILD_DIR)/samosa-fs" \
	sh tests/test_compiled_gateway.sh
	sh tests/test_settings_compact_proxy.sh
	SAMOSA_EXTRACT="$${SAMOSA_EXTRACT:-$$PWD/$(BUILD_DIR)/samosa-extract}" \
	SAMOSA_OCR="$${SAMOSA_OCR:-$$PWD/$(BUILD_DIR)/samosa-ocr}" \
	sh tests/test_attachments.sh
	SAMOSA_COMPILED_GATEWAY="$$PWD/$(BUILD_DIR)/samosa-gateway" \
	SAMOSA_FAKE_BACKEND="$$PWD/$(BUILD_DIR)/test_fake_openai_backend" \
	SAMOSA_FAKE_SUMMARIZER="$$PWD/$(BUILD_DIR)/test_fake_native_summarizer" \
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

install: omp samosa-gateway samosa-maple samosa-jobsd samosa-fs samosa-ocr chutni-service
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
	$(CC) -O2 $(CWARN) -std=c11 tools/pagecache_residency.c -o $(BUILD_DIR)/pagecache-residency

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
	$(MAKE) app-lifecycle-test
	$(MAKE) test-kimi-converter
# T3.3 view logic. Skips with a message where node is unavailable rather than
# passing silently -- the rendering itself cannot be verified headlessly.
	sh tests/test_chutni_views.sh
	sh tests/test_hidden_toggles.sh
# Backend sizing must be correct for machines this repo cannot run on, so the
# tier table is unit-tested and guarded against drifting from the gateway.
	$(CC) -O1 $(CWARN) -std=c11 tests/test_backend_limits.c -o $(BUILD_DIR)/test_backend_limits && ./$(BUILD_DIR)/test_backend_limits
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

debian-portability-test: compiled-gateway-test test-chutni-db test-chutni chutni-gateway-test

ci-debian:
	docker run --rm --platform linux/amd64 \
	  -v "$$PWD:/src:ro" \
	  debian:bookworm-slim \
	  sh -ec '\
	    apt-get update; \
	    DEBIAN_FRONTEND=noninteractive apt-get install -y \
	      make gcc libc6-dev curl python3 nodejs sqlite3 libomp-dev file ca-certificates; \
	    useradd -m ci; \
	    cp -a /src /work; \
	    chown -R ci:ci /work; \
	    su -s /bin/sh ci -c "\
	      cd /work && \
	      rm -rf build-debian && \
	      BUILD_DIR=build-debian make debian-portability-test\
	    "; \
	  '

ci-ubuntu-full:
	docker run --rm --platform linux/amd64 \
	  -v "$$PWD:/src:ro" \
	  ubuntu:latest \
	  sh -ec '\
	    apt-get update; \
	    DEBIAN_FRONTEND=noninteractive apt-get install -y \
	      make gcc libc6-dev curl python3 nodejs sqlite3 libomp-dev \
	      file ca-certificates git bash; \
	    useradd -m ci; \
	    cp -a /src /work; \
	    chown -R ci:ci /work; \
	    su -s /bin/bash ci -c "\
	      cd /work && \
	      rm -rf build && \
	      SAMOSA_ALLOW_SLOW_CPU=1 make test\
	    "; \
	  '

# Maple MLX Smoke Test (Stage A)
MLX_BUILD_DIR ?= $(BUILD_DIR)/mlx-build
MLX_INCLUDE = -Ivendor/mlx -I$(MLX_BUILD_DIR)
# MLX builds JACCL only with a sufficiently new macOS SDK; older supported
# SDKs compile no_jaccl.cpp into libmlx instead. Link the companion archive
# exactly when CMake produced it, so clean runners and newer developer SDKs
# use the same pinned source without a host-specific hardcoded assumption.
MLX_JACCL_LIB = $(wildcard $(MLX_BUILD_DIR)/jaccl/libjaccl.a)
MLX_LDFLAGS = -L$(MLX_BUILD_DIR) -lmlx $(MLX_JACCL_LIB) -framework Foundation -framework Metal -framework Accelerate -lc++
MAPLE_STREAMING_SRCS = src/maple/maple_expert_views.cpp src/maple/maple_expert_store.cpp
MAPLE_CACHE_OBJ = $(BUILD_DIR)/expert_cache.o

$(MAPLE_CACHE_OBJ): src/expert_cache.c src/expert_cache.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -std=c11 -O2 -Wall -Wextra -Isrc -c src/expert_cache.c -o $(MAPLE_CACHE_OBJ)

mlx:
	sh tools/build_mlx_native.sh

maple-mlx-smoke: tests/maple_mlx_smoke.cpp
	@mkdir -p $(BUILD_DIR)
	@test -d $(MLX_BUILD_DIR) || { echo "Run 'make mlx' first to build the native MLX library" >&2; exit 2; }
	$(CXX) -std=c++17 -O2 -Wall -Wextra $(MLX_INCLUDE) tests/maple_mlx_smoke.cpp -o $(BUILD_DIR)/maple-mlx-smoke $(MLX_LDFLAGS)
	METAL_PATH=$(MLX_BUILD_DIR)/mlx/backend/metal/kernels $(BUILD_DIR)/maple-mlx-smoke

test-maple-native: tests/test_maple_native.cpp src/maple/maple_model.cpp src/maple/tokenizer.cpp $(MAPLE_STREAMING_SRCS) $(MAPLE_CACHE_OBJ)
	@echo "Building test-maple-native..."
	@mkdir -p $(BUILD_DIR)
	@test -d $(MLX_BUILD_DIR) || { echo "Run 'make mlx' first to build the native MLX library" >&2; exit 2; }
	$(CXX) -std=c++17 -O2 -Wall -Wextra $(MLX_INCLUDE) -Isrc tests/test_maple_native.cpp src/maple/maple_model.cpp src/maple/tokenizer.cpp $(MAPLE_STREAMING_SRCS) $(MAPLE_CACHE_OBJ) -o $(BUILD_DIR)/test-maple-native $(MLX_LDFLAGS)
	METAL_PATH=$(MLX_BUILD_DIR)/mlx/backend/metal/kernels $(BUILD_DIR)/test-maple-native

test-maple-components: tests/test_maple_components.cpp src/maple/maple_model.cpp $(MAPLE_STREAMING_SRCS) $(MAPLE_CACHE_OBJ)
	@echo "Building C++ parity tests..."
	$(CXX) -std=c++17 -O2 -Wall -Wextra $(MLX_INCLUDE) -Isrc -Isrc/maple $^ -o $(BUILD_DIR)/$@ $(MLX_LDFLAGS)

test-maple-openai-messages: tests/test_maple_openai_messages.cpp src/maple/openai_message_content.h src/json.h
	@mkdir -p $(BUILD_DIR)
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Isrc tests/test_maple_openai_messages.cpp -o $(BUILD_DIR)/$@
	$(BUILD_DIR)/$@

test-maple-cache-boundary: tests/test_maple_cache_boundary.cpp src/maple/maple_model.cpp $(MAPLE_STREAMING_SRCS) $(MAPLE_CACHE_OBJ)
	@mkdir -p $(BUILD_DIR)
	$(CXX) -std=c++17 -O2 -Wall -Wextra $(MLX_INCLUDE) -Isrc -Isrc/maple $^ -o $(BUILD_DIR)/$@ $(MLX_LDFLAGS)
	$(BUILD_DIR)/$@

test-maple-model: tests/test_maple_model.cpp src/maple/maple_model.cpp $(MAPLE_STREAMING_SRCS) $(MAPLE_CACHE_OBJ)
	@echo "Building C++ model tests..."
	$(CXX) -std=c++17 -O2 -Wall -Wextra $(MLX_INCLUDE) -Isrc -Isrc/maple $^ -o $(BUILD_DIR)/$@ $(MLX_LDFLAGS)

test-maple-gate-a: tests/test_maple_gate_a.cpp src/maple/maple_model.cpp $(MAPLE_STREAMING_SRCS) $(MAPLE_CACHE_OBJ)
	@echo "Building C++ Gate A test..."
	$(CXX) -std=c++17 -O2 -Wall -Wextra $(MLX_INCLUDE) -Isrc -Isrc/maple $^ -o $(BUILD_DIR)/$@ $(MLX_LDFLAGS)

test-maple-gate-c: tests/test_maple_gate_c.cpp src/maple/maple_model.cpp $(MAPLE_STREAMING_SRCS) $(MAPLE_CACHE_OBJ)
	@echo "Building C++ Gate C test..."
	$(CXX) -std=c++17 -O2 -Wall -Wextra $(MLX_INCLUDE) -Isrc -Isrc/maple $^ -o $(BUILD_DIR)/$@ $(MLX_LDFLAGS)

test-maple: test-maple-components test-maple-model
	@echo "Running Maple C++ Components Tests"
	$(BUILD_DIR)/test-maple-components
	@echo "Running Maple C++ Model Tests"
	$(BUILD_DIR)/test-maple-model

test-maple-sanitize: tests/fixtures/maple/test_checkpoint_sanitization.py
	sh tools/run_memory_guarded.sh python3 tests/fixtures/maple/test_checkpoint_sanitization.py

test-maple-memory: tools/test_maple_memory_safety.sh
	sh tools/test_maple_memory_safety.sh

test-maple-real: test-maple-native
	@test -n "$(MAPLE_MODEL_DIR)" || { echo "set MAPLE_MODEL_DIR to point to real checkpoint" >&2; exit 2; }
	MAPLE_MODEL_DIR="$(MAPLE_MODEL_DIR)" \
	  MAX_FOOTPRINT_MB=$${MAX_FOOTPRINT_MB:-1000} \
	  MAX_SWAP_DELTA_MB=$${MAX_SWAP_DELTA_MB:-64} \
	  SAMOSA_MAPLE_EXPERT_BUDGET_MB=$${SAMOSA_MAPLE_EXPERT_BUDGET_MB:-64} \
	  METAL_PATH=$(MLX_BUILD_DIR)/mlx/backend/metal/kernels \
	  sh tools/run_memory_guarded.sh $(BUILD_DIR)/test-maple-native

test-maple-greedy-parity: tests/test_maple_greedy_parity.cpp src/maple/maple_model.cpp src/maple/tokenizer.cpp $(MAPLE_STREAMING_SRCS) $(MAPLE_CACHE_OBJ)
	@echo "Building Maple greedy streaming parity test..."
	@mkdir -p $(BUILD_DIR)
	@test -d $(MLX_BUILD_DIR) || { echo "Run 'make mlx' first to build the native MLX library" >&2; exit 2; }
	$(CXX) -std=c++17 -O2 -Wall -Wextra $(MLX_INCLUDE) -Isrc -Isrc/maple tests/test_maple_greedy_parity.cpp src/maple/maple_model.cpp src/maple/tokenizer.cpp $(MAPLE_STREAMING_SRCS) $(MAPLE_CACHE_OBJ) -o $(BUILD_DIR)/test-maple-greedy-parity $(MLX_LDFLAGS)
	MAX_FOOTPRINT_MB=$${MAX_FOOTPRINT_MB:-1000} \
	  MAX_SWAP_DELTA_MB=$${MAX_SWAP_DELTA_MB:-64} \
	  SAMOSA_MAPLE_EXPERT_BUDGET_MB=$${SAMOSA_MAPLE_EXPERT_BUDGET_MB:-64} \
	  METAL_PATH=$(MLX_BUILD_DIR)/mlx/backend/metal/kernels \
	  sh tools/run_memory_guarded.sh $(BUILD_DIR)/test-maple-greedy-parity

test-maple-streamed-logits: tests/test_maple_streamed_logits.cpp src/maple/maple_model.cpp src/maple/tokenizer.cpp $(MAPLE_STREAMING_SRCS) $(MAPLE_CACHE_OBJ)
	@echo "Building Maple streamed raw-logit parity test..."
	@mkdir -p $(BUILD_DIR)
	@test -d $(MLX_BUILD_DIR) || { echo "Run 'make mlx' first to build the native MLX library" >&2; exit 2; }
	$(CXX) -std=c++17 -O2 -Wall -Wextra $(MLX_INCLUDE) -Isrc -Isrc/maple tests/test_maple_streamed_logits.cpp src/maple/maple_model.cpp src/maple/tokenizer.cpp $(MAPLE_STREAMING_SRCS) $(MAPLE_CACHE_OBJ) -o $(BUILD_DIR)/test-maple-streamed-logits $(MLX_LDFLAGS)
	MAX_FOOTPRINT_MB=$${MAX_FOOTPRINT_MB:-1000} \
	  MAX_SWAP_DELTA_MB=$${MAX_SWAP_DELTA_MB:-64} \
	  SAMOSA_MAPLE_EXPERT_BUDGET_MB=$${SAMOSA_MAPLE_EXPERT_BUDGET_MB:-64} \
	  METAL_PATH=$(MLX_BUILD_DIR)/mlx/backend/metal/kernels \
	  sh tools/run_memory_guarded.sh $(BUILD_DIR)/test-maple-streamed-logits

test-maple-reference-live:
	@echo "WARNING: This command may load the complete upstream Maple model."
	@echo "Memory guard is active. (8GB Limit)"
	@test -n "$(MAPLE_MODEL_DIR)" || { echo "set MAPLE_MODEL_DIR to point to real checkpoint" >&2; exit 2; }
	MAPLE_MODEL_DIR="$(MAPLE_MODEL_DIR)" MAX_FOOTPRINT_MB=8192 MAX_SWAP_DELTA_MB=256 sh tools/run_memory_guarded.sh python3 tools/maple_reference.py

test-maple-parity: samosa-maple tests/fixtures/maple/361db5da5e74ff6fcdd852d478e1f266ce11013a/maple_parity_test.py
	python3 tests/fixtures/maple/361db5da5e74ff6fcdd852d478e1f266ce11013a/maple_parity_test.py


samosa-maple: src/maple/samosa_maple.cpp src/maple/maple_model.cpp src/maple/tokenizer.cpp $(MAPLE_STREAMING_SRCS) $(MAPLE_CACHE_OBJ)
	@echo "Building samosa-maple HTTP server..."
	@mkdir -p $(BUILD_DIR)
	@test -d $(MLX_BUILD_DIR) || { echo "Run 'make mlx' first to build the native MLX library" >&2; exit 2; }
	$(CXX) -std=c++17 -O2 -Wall -Wextra $(MLX_INCLUDE) -Isrc src/maple/samosa_maple.cpp src/maple/maple_model.cpp src/maple/tokenizer.cpp $(MAPLE_STREAMING_SRCS) $(MAPLE_CACHE_OBJ) -o $(BUILD_DIR)/samosa-maple $(MLX_LDFLAGS)

maple-pack: tools/maple_pack.cpp src/maple/maple_expert_store.cpp src/maple/maple_expert_store.h src/json.h $(MAPLE_CACHE_OBJ)
	@echo "Building maple-pack..."
	@mkdir -p $(BUILD_DIR)
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Isrc tools/maple_pack.cpp src/maple/maple_expert_store.cpp $(MAPLE_CACHE_OBJ) -o $(BUILD_DIR)/maple-pack

test-maple-streaming-quarantine: samosa-gateway
	sh tests/test_maple_streaming_quarantine.sh

test-maple-expert-store: tests/test_maple_expert_store.cpp src/maple/maple_expert_store.cpp src/maple/maple_expert_store.h src/expert_cache.h src/json.h $(MAPLE_CACHE_OBJ)
	@echo "Building Maple expert store tests..."
	@mkdir -p $(BUILD_DIR)
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Isrc tests/test_maple_expert_store.cpp src/maple/maple_expert_store.cpp $(MAPLE_CACHE_OBJ) -o $(BUILD_DIR)/test-maple-expert-store
	$(BUILD_DIR)/test-maple-expert-store

test-maple-expert-views: tests/test_maple_expert_views.cpp $(MAPLE_STREAMING_SRCS) src/maple/maple_model.cpp src/expert_cache.h src/json.h $(MAPLE_CACHE_OBJ)
	@echo "Building Maple MLX expert-view tests..."
	@mkdir -p $(BUILD_DIR)
	@test -d $(MLX_BUILD_DIR) || { echo "Run 'make mlx' first to build the native MLX library" >&2; exit 2; }
	$(CXX) -std=c++17 -O2 -Wall -Wextra $(MLX_INCLUDE) -Isrc tests/test_maple_expert_views.cpp $(MAPLE_STREAMING_SRCS) src/maple/maple_model.cpp $(MAPLE_CACHE_OBJ) -o $(BUILD_DIR)/test-maple-expert-views $(MLX_LDFLAGS)
	METAL_PATH=$(MLX_BUILD_DIR)/mlx/backend/metal/kernels $(BUILD_DIR)/test-maple-expert-views

test_attn_parity: tests/test_attn_parity.cpp src/maple/maple_model.cpp $(MAPLE_STREAMING_SRCS) $(MAPLE_CACHE_OBJ)
	$(CXX) -std=c++17 -O2 -Wall -Wextra $(MLX_INCLUDE) -Isrc $^ -o build/$@ $(MLX_LDFLAGS)

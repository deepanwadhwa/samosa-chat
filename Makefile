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

# read-cache-test: offline gate for the content-addressed doc.read cache (R4).
read-cache-test: tests/test_read_cache.c src/read_cache.h src/json.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Wno-unused-function -std=c11 tests/test_read_cache.c -o $(BUILD_DIR)/test_read_cache
	$(BUILD_DIR)/test_read_cache

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

samosa-gateway: src/samosa_gateway.c src/samosa_http.h src/json.h
	@mkdir -p $(BUILD_DIR)
	$(CC) -O2 -Wall -Wextra -Werror -Wno-unused-function -std=c11 -pthread -Isrc \
	  src/samosa_gateway.c -o $(BUILD_DIR)/samosa-gateway

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

compiled-gateway-test: samosa-gateway samosa-jobsd samosa-fs test_fake_openai_backend tests/test_compiled_gateway.sh
	SAMOSA_COMPILED_GATEWAY="$$PWD/$(BUILD_DIR)/samosa-gateway" \
	SAMOSA_COMPILED_JOBSD="$$PWD/$(BUILD_DIR)/samosa-jobsd" \
	SAMOSA_FAKE_BACKEND="$$PWD/$(BUILD_DIR)/test_fake_openai_backend" \
	SAMOSA_FS="$$PWD/$(BUILD_DIR)/samosa-fs" sh tests/test_compiled_gateway.sh

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

test: pagecache-residency-test tests/test_expert_cache.c tests/test_kv_cache.c tests/test_repetition_guard.c tests/test_thinking_budget.c tests/test_groupwise_q4.c tests/test_samosa_serve.c tests/test_samosa_wrapper.sh tests/test_atomic_install.sh tests/test_install_path.sh tests/test_gateway_installer.sh tests/test_thinking_output.py tests/test_regression_gate.py tests/test_openrouter_control.py tests/test_route_analysis.py tests/test_spec_accept.py tests/test_converter_quant.py tests/test_package_pdfium.py
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
	python3 tests/test_thinking_output.py
	python3 tests/test_regression_gate.py
	python3 tests/test_openrouter_control.py
	python3 tests/test_route_analysis.py
	python3 tests/test_spec_accept.py
	python3 tests/test_package_pdfium.py
	@if [ -n "$(NUMPY_PYTHON)" ]; then $(NUMPY_PYTHON) tests/test_converter_quant.py; \
	else echo "converter quant tests: SKIP (NumPy environment unavailable)"; fi

# Jobs acceptance (offline). Gate 11 removed the Python jobs modules
# (samosa_jobs/samosa_gateway/samosa_tools/jobs_fs) after native parity, so the
# Jobs runtime under test is the compiled gateway/jobsd/fs. The shipped samosa-fs
# sidecar has direct CLI coverage in tests/jobs/, and every C job route (chat,
# run/find/answer, definition preview/run, move/apply/undo, schedule/jobsd,
# launchd, public-inputs, kill) is exercised by tests/test_compiled_gateway.sh
# with python3 removed from PATH.
jobs-test: samosa-fs
	SAMOSA_FS="$$PWD/$(BUILD_DIR)/samosa-fs" python3 -m unittest discover -s tests/jobs -v
	$(MAKE) compiled-gateway-test

clean:
	rm -rf $(BUILD_DIR)

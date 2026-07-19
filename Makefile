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
	$(CC) -O3 -Wno-unused-function -pthread src/qwen36b.c src/expert_cache.c src/vision.c -o qwen36b -lm

samosa-extract: src/samosa_extract.c src/tok.h src/tok_unicode.h src/json.h $(PDFIUM_READY)
	@if [ -z "$(PDFIUM_DIR)" ]; then \
	  echo "PDFium support unavailable: set PDFIUM_DIR to an unpacked PDFium artifact" >&2; exit 2; \
	fi
	@if [ -z "$(PDFIUM_LIBRARY)" ]; then \
	  echo "PDFium support unavailable: no libpdfium shared library under $(PDFIUM_DIR)/lib" >&2; exit 2; \
	fi
	$(CC) -O2 -Wall -Wextra -Werror -Wno-unused-function -std=c11 -I$(PDFIUM_DIR)/include \
	  src/samosa_extract.c $(PDFIUM_LIBRARY) \
	  -Wl,-rpath,$(PDFIUM_DIR)/lib -o samosa-extract
	@if [ "$(UNAME_S)" = "Darwin" ]; then \
	  install_name_tool -change ./libpdfium.dylib @rpath/libpdfium.dylib samosa-extract; \
	fi

extract-test: samosa-extract tests/test_samosa_extract.sh tests/fixtures/documents/hello.pdf
	SAMOSA_EXTRACT=./samosa-extract sh tests/test_samosa_extract.sh

extract-tokenizer-test: samosa-extract tests/test_samosa_extract.sh
	@test -n "$(SAMOSA_EXTRACT_TOKENIZER)" || { echo "set SAMOSA_EXTRACT_TOKENIZER to run exact-token tests" >&2; exit 2; }
	SAMOSA_EXTRACT=./samosa-extract SAMOSA_EXTRACT_TOKENIZER="$(SAMOSA_EXTRACT_TOKENIZER)" sh tests/test_samosa_extract.sh

document-installer-test: tests/test_document_installer.sh
	sh tests/test_document_installer.sh

omp: src/qwen36b.c src/expert_cache.c src/vision.c $(ENGINE_HEADERS)
	$(CC) -O3 -Wno-unused-function -pthread $(OMP_CFLAGS) \
	  src/qwen36b.c src/expert_cache.c src/vision.c -o qwen36b -lm $(OMP_LDFLAGS)

# E-X5 experiment build only — never shipped. Same as `omp` plus
# -DSAMOSA_SCHED_RUNTIME, so OMP_SCHEDULE picks the hot-kernel schedule at run
# time. Separate output name so it can never be installed by mistake.
omp-sched-runtime: src/qwen36b.c src/expert_cache.c src/vision.c $(ENGINE_HEADERS)
	$(CC) -O3 -Wno-unused-function -pthread $(OMP_CFLAGS) -DSAMOSA_SCHED_RUNTIME \
	  src/qwen36b.c src/expert_cache.c src/vision.c -o qwen36b-sched-runtime -lm $(OMP_LDFLAGS)

# E-X10 M0 experiment only — never linked into or installed as qwen36b.
# This target is intentionally Darwin-only: it exercises Apple's Metal API
# and compares the shader with the exact NEON/OpenMP grouped-q4 reference.
metal-spike: tools/metal_spike.m src/kernels.h
	@if [ "$(UNAME_S)" != "Darwin" ]; then \
	  echo "metal-spike requires macOS and Apple Metal" >&2; exit 2; \
	fi
	$(CC) -O3 -Wall -Wextra -Wno-unused-function -Wno-unknown-pragmas \
	  -fobjc-arc -pthread $(OMP_CFLAGS) -Isrc tools/metal_spike.m \
	  -o metal-spike -framework Foundation -framework Metal -lm $(OMP_LDFLAGS)

# E-X10 M1 system experiment — separate binary, opt-in again at runtime with
# SAMOSA_METAL=1. It keeps the normal qwen36b and installer CPU-only.
metal-omp: src/qwen36b.c src/expert_cache.c src/vision.c src/metal_expert.m $(ENGINE_HEADERS)
	@if [ "$(UNAME_S)" != "Darwin" ]; then \
	  echo "metal-omp requires macOS and Apple Metal" >&2; exit 2; \
	fi
	$(CC) -O3 -Wno-unused-function -Wno-unknown-pragmas -pthread \
	  $(OMP_CFLAGS) -DSAMOSA_METAL -fobjc-arc \
	  src/qwen36b.c src/expert_cache.c src/vision.c src/metal_expert.m \
	  -o qwen36b-metal -framework Foundation -framework Metal -lm $(OMP_LDFLAGS)

pagecache-residency: tools/pagecache_residency.c
	$(CC) -O2 -Wall -Wextra -Werror -std=c11 tools/pagecache_residency.c -o pagecache-residency

pagecache-residency-test: pagecache-residency tests/test_pagecache_residency.sh
	sh tests/test_pagecache_residency.sh ./pagecache-residency

test: pagecache-residency-test tests/test_expert_cache.c tests/test_kv_cache.c tests/test_repetition_guard.c tests/test_thinking_budget.c tests/test_groupwise_q4.c tests/test_samosa_serve.c tests/test_samosa_wrapper.sh tests/test_gateway_web.py tests/test_atomic_install.sh tests/test_install_path.sh tests/test_thinking_output.py tests/test_regression_gate.py tests/test_openrouter_control.py tests/test_route_analysis.py tests/test_spec_accept.py tests/test_converter_quant.py tests/test_package_pdfium.py
	$(CC) -O1 -Isrc tests/test_expert_cache.c src/expert_cache.c -o test_expert_cache && ./test_expert_cache
	$(CC) -O1 -Itests tests/test_kv_cache.c tests/kv_cache.c -o test_kv_cache -lm && ./test_kv_cache
	$(CC) -O1 -Isrc tests/test_repetition_guard.c -o test_repetition_guard && ./test_repetition_guard
	$(CC) -O1 -Isrc tests/test_thinking_budget.c -o test_thinking_budget && ./test_thinking_budget
	$(CC) -O1 -Isrc tests/test_groupwise_q4.c -o test_groupwise_q4 -lm && ./test_groupwise_q4
	$(CC) -O1 -pthread -Isrc tests/test_samosa_serve.c src/expert_cache.c src/vision.c -o test_samosa_serve -lm && ./test_samosa_serve
	sh tests/test_samosa_wrapper.sh
	python3 tests/test_gateway_web.py
	sh tests/test_atomic_install.sh
	sh tests/test_install_path.sh
	python3 tests/test_thinking_output.py
	python3 tests/test_regression_gate.py
	python3 tests/test_openrouter_control.py
	python3 tests/test_route_analysis.py
	python3 tests/test_spec_accept.py
	python3 tests/test_package_pdfium.py
	@if [ -n "$(NUMPY_PYTHON)" ]; then $(NUMPY_PYTHON) tests/test_converter_quant.py; \
	else echo "converter quant tests: SKIP (NumPy environment unavailable)"; fi

clean:
	rm -f qwen36b qwen36b-metal qwen36b-sched-runtime metal-spike samosa-extract pagecache-residency test_expert_cache test_kv_cache test_repetition_guard test_thinking_budget test_groupwise_q4 test_samosa_serve

# Samosa Chat — build the engine. `make` for the portable build; `make omp` for
# the multithreaded build (brew install libomp first).
CC ?= clang
OMP_PREFIX := $(shell [ -d /opt/homebrew/opt/libomp ] && echo /opt/homebrew/opt/libomp || echo /usr/local/opt/libomp)
NUMPY_PYTHON := $(shell python3 -c 'import numpy' >/dev/null 2>&1 && echo python3 || { [ -x ../.venv/bin/python ] && echo ../.venv/bin/python; })
ENGINE_HEADERS := $(wildcard src/*.h)

samosa-engine: src/qwen36b.c src/expert_cache.c $(ENGINE_HEADERS)
	$(CC) -O3 -Wno-unused-function -pthread src/qwen36b.c src/expert_cache.c -o qwen36b -lm

omp: src/qwen36b.c src/expert_cache.c $(ENGINE_HEADERS)
	$(CC) -O3 -Wno-unused-function -pthread -Xclang -fopenmp -I$(OMP_PREFIX)/include \
	  src/qwen36b.c src/expert_cache.c -o qwen36b -lm -L$(OMP_PREFIX)/lib -lomp

test: tests/test_expert_cache.c tests/test_kv_cache.c tests/test_repetition_guard.c tests/test_thinking_budget.c tests/test_groupwise_q4.c tests/test_samosa_serve.c tests/test_samosa_wrapper.sh tests/test_atomic_install.sh tests/test_install_path.sh tests/test_thinking_output.py tests/test_regression_gate.py tests/test_openrouter_control.py tests/test_route_analysis.py tests/test_converter_quant.py
	$(CC) -O1 -Isrc tests/test_expert_cache.c src/expert_cache.c -o test_expert_cache && ./test_expert_cache
	$(CC) -O1 -Itests tests/test_kv_cache.c tests/kv_cache.c -o test_kv_cache -lm && ./test_kv_cache
	$(CC) -O1 -Isrc tests/test_repetition_guard.c -o test_repetition_guard && ./test_repetition_guard
	$(CC) -O1 -Isrc tests/test_thinking_budget.c -o test_thinking_budget && ./test_thinking_budget
	$(CC) -O1 -Isrc tests/test_groupwise_q4.c -o test_groupwise_q4 -lm && ./test_groupwise_q4
	$(CC) -O1 -pthread -Isrc tests/test_samosa_serve.c src/expert_cache.c -o test_samosa_serve -lm && ./test_samosa_serve
	sh tests/test_samosa_wrapper.sh
	sh tests/test_atomic_install.sh
	sh tests/test_install_path.sh
	python3 tests/test_thinking_output.py
	python3 tests/test_regression_gate.py
	python3 tests/test_openrouter_control.py
	python3 tests/test_route_analysis.py
	@if [ -n "$(NUMPY_PYTHON)" ]; then $(NUMPY_PYTHON) tests/test_converter_quant.py; \
	else echo "converter quant tests: SKIP (NumPy environment unavailable)"; fi

clean:
	rm -f qwen36b test_expert_cache test_kv_cache test_repetition_guard test_thinking_budget test_groupwise_q4 test_samosa_serve

# Samosa Chat — build the engine. `make` for the portable build; `make omp` for
# the multithreaded build (brew install libomp first).
CC ?= clang
OMP_PREFIX := $(shell [ -d /opt/homebrew/opt/libomp ] && echo /opt/homebrew/opt/libomp || echo /usr/local/opt/libomp)

samosa-engine: src/qwen36b.c src/expert_cache.c
	$(CC) -O3 -Wno-unused-function src/qwen36b.c src/expert_cache.c -o qwen36b -lm

omp: src/qwen36b.c src/expert_cache.c
	$(CC) -O3 -Wno-unused-function -Xclang -fopenmp -I$(OMP_PREFIX)/include \
	  src/qwen36b.c src/expert_cache.c -o qwen36b -lm -L$(OMP_PREFIX)/lib -lomp

test: tests/test_expert_cache.c tests/test_kv_cache.c
	$(CC) -O1 -Isrc tests/test_expert_cache.c src/expert_cache.c -o test_expert_cache && ./test_expert_cache
	$(CC) -O1 -Itests tests/test_kv_cache.c tests/kv_cache.c -o test_kv_cache && ./test_kv_cache

clean:
	rm -f qwen36b test_expert_cache test_kv_cache

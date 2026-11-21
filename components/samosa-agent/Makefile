CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -O2 -g
WARN = -std=c11 -Wall -Wextra -Werror -Wpedantic
HEADERS = src/agent.h src/command.h src/json.h src/store.h src/sha256.h src/context.h src/evidence.h src/find.h src/actions.h src/reconcile.h src/child.h src/budget.h src/source_call.h
SOURCES = src/agent.c src/command.c src/store.c src/context.c src/evidence.c src/find.c src/actions.c src/reconcile.c src/child.c src/budget.c src/source_call.c
LIBS = -lsqlite3
.PHONY: test-concurrent sanitize-concurrent
build/test-concurrent: tests/test_concurrent.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_AGENT_TEST_FAULTS -Isrc tests/test_concurrent.c $(SOURCES) $(LIBS) -o $@
test-concurrent: build/test-concurrent
	./build/test-concurrent
sanitize-concurrent: build/test-concurrent
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -DSAMOSA_AGENT_TEST_FAULTS -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_concurrent.c $(SOURCES) $(LIBS) -o build/test-concurrent-sanitize
	SAMOSA_QA_CONCURRENT_PROVIDER='$(CURDIR)/build/test-concurrent' ./build/test-concurrent-sanitize
.PHONY: all test test-interaction sanitize sanitize-actions release clean
.PHONY: test-find-interaction
.PHONY: test-actions-interaction
.PHONY: test-cross-device
build/test-cross-device: tests/cross_device.c src/command.c src/command.h src/json.h src/sha256.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/cross_device.c src/command.c -o $@
test-cross-device: all build/test-cross-device
	./build/test-cross-device ./build/samosa-agent
build/actions-interaction: tests/actions_interaction.c src/command.c src/command.h src/json.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/actions_interaction.c src/command.c -o $@
test-actions-interaction: all build/actions-interaction
	@test -n '$(SOURCES_EXECUTABLE)' || { echo 'configure external SOURCES_EXECUTABLE'; exit 2; }
	./build/actions-interaction ./build/samosa-agent '$(SOURCES_EXECUTABLE)'
test-find-interaction: all
	@test -n '$(SOURCES_EXECUTABLE)' || { echo 'configure external SOURCES_EXECUTABLE'; exit 2; }
	sh tests/find_interaction.sh ./build/samosa-agent '$(SOURCES_EXECUTABLE)'
all: build/samosa-agent
build/test-reconcile: tests/test_reconcile.c src/reconcile.c src/reconcile.h src/command.c src/command.h src/json.h src/sha256.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_reconcile.c src/reconcile.c src/command.c -o $@
test-reconcile: build/test-reconcile
	./build/test-reconcile
build/test-handoff: tests/test_handoff.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_handoff.c $(SOURCES) $(LIBS) -o $@
test-handoff: build/test-handoff
	./build/test-handoff
build:
	mkdir -p build
build/samosa-agent: src/main.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) src/main.c $(SOURCES) $(LIBS) -o $@
build/test-agent: tests/test_agent.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_AGENT_TEST_FAULTS -Isrc tests/test_agent.c $(SOURCES) $(LIBS) -o $@
build/test-find: tests/test_find.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_find.c $(SOURCES) $(LIBS) -o $@
build/test-actions: tests/test_actions.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_AGENT_TEST_FAULTS -Isrc tests/test_actions.c $(SOURCES) $(LIBS) -o $@
build/test-undo: tests/test_undo.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_AGENT_TEST_FAULTS -Isrc tests/test_undo.c $(SOURCES) $(LIBS) -o $@
build/test-move: tests/test_move.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_AGENT_TEST_FAULTS -Isrc tests/test_move.c $(SOURCES) $(LIBS) -o $@
test: all build/test-agent build/test-find build/test-actions build/test-undo build/test-move build/test-reconcile build/test-handoff
	./build/test-reconcile
	./build/test-handoff
	./build/test-agent
	./build/test-find
	./build/test-actions
	./build/test-undo
	./build/test-move
test-interaction: all
	@test -n '$(SOURCES_EXECUTABLE)' -a -n '$(INFERENCE_EXECUTABLE)' || { echo 'configure external SOURCES_EXECUTABLE and INFERENCE_EXECUTABLE'; exit 2; }
	sh tests/interaction.sh ./build/samosa-agent '$(SOURCES_EXECUTABLE)' '$(INFERENCE_EXECUTABLE)'
sanitize-actions: | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -DSAMOSA_AGENT_TEST_FAULTS -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_move.c $(SOURCES) $(LIBS) -o build/test-move-sanitize
	./build/test-move-sanitize
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -DSAMOSA_AGENT_TEST_FAULTS -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_undo.c $(SOURCES) $(LIBS) -o build/test-undo-sanitize
	./build/test-undo-sanitize
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -DSAMOSA_AGENT_TEST_FAULTS -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_actions.c $(SOURCES) $(LIBS) -o build/test-actions-sanitize
	./build/test-actions-sanitize
sanitize: sanitize-actions
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_handoff.c $(SOURCES) $(LIBS) -o build/test-handoff-sanitize
	./build/test-handoff-sanitize
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_reconcile.c src/reconcile.c src/command.c -o build/test-reconcile-sanitize
	./build/test-reconcile-sanitize
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_find.c $(SOURCES) $(LIBS) -o build/test-find-sanitize
	./build/test-find-sanitize
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -DSAMOSA_AGENT_TEST_FAULTS -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_agent.c $(SOURCES) $(LIBS) -o build/test-agent-sanitize
	./build/test-agent-sanitize
release: test
	mkdir -p dist
	tar -czf dist/samosa-agent-0.1.0.tar.gz Makefile README.md DEPENDENCIES.md .gitignore src tests docs contracts
clean:
	find build dist -depth -delete

.PHONY: test-native
test-native: all
	@test -n '$(SOURCES_EXECUTABLE)' -a -n '$(INFERENCE_EXECUTABLE)' -a -n '$(PDF_FIXTURE_EXECUTABLE)' -a -n '$(PDFIUM_LIBRARY)' -a -n '$(LLAMA_ENDPOINT)' || { echo 'configure external Sources/Inference/PDF fixture/PDFium/llama endpoint'; exit 2; }
	sh tests/native_smoke.sh ./build/samosa-agent '$(SOURCES_EXECUTABLE)' '$(INFERENCE_EXECUTABLE)' '$(PDF_FIXTURE_EXECUTABLE)' '$(PDFIUM_LIBRARY)' '$(LLAMA_ENDPOINT)'

.PHONY: test-conversation-native
test-conversation-native: all
	@test -n '$(SOURCES_EXECUTABLE)' -a -n '$(INFERENCE_EXECUTABLE)' -a -n '$(LLAMA_ENDPOINT)' || { echo 'configure external Sources/Inference/llama endpoint'; exit 2; }
	sh tests/conversation_native.sh ./build/samosa-agent '$(SOURCES_EXECUTABLE)' '$(INFERENCE_EXECUTABLE)' '$(LLAMA_ENDPOINT)'

.PHONY: test-session-native
test-session-native: all
	@test -n '$(SOURCES_EXECUTABLE)' -a -n '$(INFERENCE_EXECUTABLE)' -a -n '$(ENGINE_LAUNCHER)' -a -n '$(LLAMA_SERVER)' -a -n '$(MODEL_PATH)' -a -n '$(TEST_PORT)' || { echo 'configure external Sources/Inference/native launcher/engine/model and unused TEST_PORT'; exit 2; }
	sh tests/session_native.sh ./build/samosa-agent '$(SOURCES_EXECUTABLE)' '$(INFERENCE_EXECUTABLE)' '$(ENGINE_LAUNCHER)' '$(LLAMA_SERVER)' '$(MODEL_PATH)' '$(TEST_PORT)'

.PHONY: test-run-cancel sanitize-run-cancel
build/test-run-cancel: tests/test_run_cancel.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_AGENT_TEST_FAULTS -Isrc tests/test_run_cancel.c $(SOURCES) $(LIBS) -o $@
test-run-cancel: build/test-run-cancel
	./build/test-run-cancel
sanitize-run-cancel: | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -DSAMOSA_AGENT_TEST_FAULTS -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_run_cancel.c $(SOURCES) $(LIBS) -o build/test-run-cancel-sanitize
	./build/test-run-cancel-sanitize
test: test-run-cancel
sanitize: sanitize-run-cancel

.PHONY: test-child-operations sanitize-child-operations
build/test-child-operations: tests/test_child_operations.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_AGENT_TEST_FAULTS -Isrc tests/test_child_operations.c $(SOURCES) $(LIBS) -o $@
test-child-operations: build/test-child-operations
	./build/test-child-operations
sanitize-child-operations: build/test-child-operations | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -DSAMOSA_AGENT_TEST_FAULTS -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_child_operations.c $(SOURCES) $(LIBS) -o build/test-child-operations-sanitize
	SAMOSA_QA_CHILD_PROVIDER='$(CURDIR)/build/test-child-operations' ./build/test-child-operations-sanitize
test: test-child-operations
sanitize: sanitize-child-operations

CURL_CFLAGS ?= $(shell curl-config --cflags)
CURL_LIBS ?= $(shell curl-config --libs)
build/child-native: tests/child_native.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) $(CURL_CFLAGS) -Isrc tests/child_native.c $(SOURCES) $(LIBS) $(CURL_LIBS) -o $@
build/child-native-sanitize: tests/child_native.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) $(CURL_CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/child_native.c $(SOURCES) $(LIBS) $(CURL_LIBS) -o $@

.PHONY: test-budget sanitize-budget
build/test-budget: tests/test_budget.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -Isrc tests/test_budget.c $(SOURCES) $(LIBS) -o $@
test-budget: build/test-budget
	./build/test-budget
sanitize-budget: build/test-budget
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_budget.c $(SOURCES) $(LIBS) -o build/test-budget-sanitize
	./build/test-budget-sanitize
test: test-budget
sanitize: sanitize-budget

.PHONY: test-source-control sanitize-source-control
build/source-control: tests/source_control.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_AGENT_TEST_FAULTS -Isrc tests/source_control.c $(SOURCES) $(LIBS) -o $@
build/source-control-sanitize: tests/source_control.c $(SOURCES) $(HEADERS) | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -DSAMOSA_AGENT_TEST_FAULTS -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/source_control.c $(SOURCES) $(LIBS) -o $@
test-source-control: build/source-control
	@test -n '$(SOURCES_EXECUTABLE)' -a -n '$(INFERENCE_EXECUTABLE)' -a -n '$(PDF_FIXTURE_EXECUTABLE)' -a -n '$(PDFIUM_LIBRARY)' -a -n '$(PDF_SPY_LIBRARY)' || { echo 'configure external Sources/Inference/PDF fixture/PDFium/spy'; exit 2; }
	./build/source-control '$(SOURCES_EXECUTABLE)' '$(INFERENCE_EXECUTABLE)' '$(PDF_FIXTURE_EXECUTABLE)' '$(PDFIUM_LIBRARY)' '$(PDF_SPY_LIBRARY)'
sanitize-source-control: build/source-control-sanitize
	@test -n '$(SOURCES_EXECUTABLE)' -a -n '$(INFERENCE_EXECUTABLE)' -a -n '$(PDF_FIXTURE_EXECUTABLE)' -a -n '$(PDFIUM_LIBRARY)' -a -n '$(PDF_SPY_LIBRARY)' || { echo 'configure external Sources/Inference/PDF fixture/PDFium/spy'; exit 2; }
	./build/source-control-sanitize '$(SOURCES_EXECUTABLE)' '$(INFERENCE_EXECUTABLE)' '$(PDF_FIXTURE_EXECUTABLE)' '$(PDFIUM_LIBRARY)' '$(PDF_SPY_LIBRARY)'

.PHONY: test-transport sanitize-transport
build/test-transport: tests/test_transport.c src/command.c src/command.h src/json.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARN) -DSAMOSA_AGENT_TEST_FAULTS -Isrc tests/test_transport.c src/command.c -o $@
test-transport: build/test-transport
	./build/test-transport
sanitize-transport: | build
	$(CC) $(CPPFLAGS) -O1 -g $(WARN) -DSAMOSA_AGENT_TEST_FAULTS -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc tests/test_transport.c src/command.c -o build/test-transport-sanitize
	./build/test-transport-sanitize
test: test-transport
sanitize: sanitize-transport

.PHONY: sanitize-source-control-unconfirmed
sanitize-source-control-unconfirmed: build/source-control-sanitize
	@test -n '$(SOURCES_EXECUTABLE)' -a -n '$(INFERENCE_EXECUTABLE)' -a -n '$(PDF_FIXTURE_EXECUTABLE)' -a -n '$(PDFIUM_LIBRARY)' -a -n '$(PDF_SPY_LIBRARY)' || { echo 'configure external Sources/Inference/PDF fixture/PDFium/spy'; exit 2; }
	./build/source-control-sanitize '$(SOURCES_EXECUTABLE)' '$(INFERENCE_EXECUTABLE)' '$(PDF_FIXTURE_EXECUTABLE)' '$(PDFIUM_LIBRARY)' '$(PDF_SPY_LIBRARY)' --allow-unconfirmed

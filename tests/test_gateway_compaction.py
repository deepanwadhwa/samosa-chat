#!/usr/bin/env python3
import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
from unittest import mock


with tempfile.TemporaryDirectory() as temp:
    root = Path(temp)
    os.environ.update({
        "SAMOSA_HOME": temp,
        "SAMOSA_APP_HTML": str(root / "app.html"),
        "SAMOSA_APP_LOGO": str(root / "logo.png"),
        "SAMOSA_QWEN_ENGINE": str(root / "qwen36b"),
        "SAMOSA_QWEN_MODEL": str(root / "model"),
        "SAMOSA_TOKENIZER": str(root / "tokenizer.json"),
    })
    spec = importlib.util.spec_from_file_location(
        "samosa_gateway_compaction",
        Path(__file__).parents[1] / "tools/samosa_gateway.py",
    )
    gateway = importlib.util.module_from_spec(spec)
    assert spec.loader
    spec.loader.exec_module(gateway)

    # Settings are validated, durable, and directly control llama-server -c.
    response, restart = gateway.settings.update({
        "context_tokens": "4096",
        "auto_compact": False,
        "compact_threshold_percent": 75,
    })
    assert restart and response["context_limit_tokens"] == 4096
    assert response["model_context_limit_tokens"] == 262144
    assert json.loads(gateway.GATEWAY_SETTINGS_FILE.read_text()) == {
        "context_tokens": "4096",
        "auto_compact": False,
        "compact_threshold_percent": 75,
    }
    gateway.supervisor.backend = "ornith"
    command, _ = gateway.supervisor.command("ornith")
    assert command[command.index("-c") + 1] == "4096"
    assert command[command.index("--fit-target") + 1] == "4096"
    gateway.settings.context_spec = "auto"
    command, _ = gateway.supervisor.command("ornith")
    assert "-c" not in command and "--ctx-size" not in command
    gateway.supervisor.runtime_context_tokens = 0
    with mock.patch.object(
        gateway, "backend_json",
        return_value=(200, {"default_generation_settings": {"n_ctx": 57344}}),
    ):
        assert gateway.supervisor.gguf_context_limit() == 57344
    assert gateway.supervisor.runtime_context_tokens == 57344
    gateway.settings.context_spec = "4096"
    for bad in ("0", "-1", "262145", "lots"):
        try:
            gateway.settings.update({"context_tokens": bad})
        except ValueError:
            pass
        else:
            raise AssertionError(f"accepted invalid context setting {bad}")

    # A per-model ledger survives a fresh object (the gateway restart path).
    messages = [
        {"role": "user", "content": "My launch code is ORBIT-731."},
        {"role": "assistant", "content": "I will remember ORBIT-731."},
    ]
    gateway.ledger.save("chat-one", "ornith", messages)
    reloaded = gateway.ConversationLedger().load("chat-one", "ornith")
    assert reloaded == messages
    assert gateway.ConversationLedger().load("chat-one", "bonsai") == []
    corrupt = gateway.ledger.path("chat-corrupt", "ornith")
    corrupt.parent.mkdir(parents=True)
    corrupt.write_text("{not-json")
    try:
        gateway.ledger.load("chat-corrupt", "ornith")
    except RuntimeError as error:
        assert "corrupt" in str(error)
    else:
        raise AssertionError("silently accepted a corrupt durable ledger")

    # Once a ledger exists, a browser may submit its full visible transcript:
    # only the newest user turn is appended to model-facing durable context.
    payload = {"conversation_id": "chat-one", "messages": [
        *messages,
        {"role": "user", "content": "What is my launch code?"},
    ]}
    conversation_id, pending = gateway.durable_messages_for_request(payload, "ornith")
    assert conversation_id == "chat-one"
    assert pending == [*messages, {"role": "user", "content": "What is my launch code?"}]

    # Exact usage must be based on /apply-template followed by /tokenize.
    calls = []
    def fake_backend(method, path, payload=None, timeout=None):
        calls.append((method, path, payload))
        if path == "/apply-template":
            return 200, {"prompt": "rendered prompt"}
        return 200, {"tokens": [1, 2, 3, 4]}
    with mock.patch.object(gateway, "backend_json", side_effect=fake_backend):
        assert gateway.exact_message_tokens(messages) == 4
    assert [call[1] for call in calls] == ["/apply-template", "/tokenize"]

    # Compaction retains a recent message-aligned tail and refuses a non-shrink.
    long_messages = [
        {"role": "user", "content": "fact " + ("alpha " * 80)},
        {"role": "assistant", "content": "noted " + ("beta " * 80)},
        {"role": "user", "content": "constraint " + ("gamma " * 80)},
        {"role": "assistant", "content": "accepted " + ("delta " * 80)},
        {"role": "user", "content": "continue from here"},
    ]
    def word_tokens(value, template_kwargs=None):
        return sum(len(message["content"].split()) + 3 for message in value)
    with mock.patch.object(gateway, "exact_message_tokens", side_effect=word_tokens), \
         mock.patch.object(gateway, "summary_text",
                           return_value="Launch fact alpha; constraint gamma remains active."):
        compacted, stats = gateway.compact_messages(long_messages, 512)
    assert compacted[0]["role"] == "system"
    assert "Launch fact alpha" in compacted[0]["content"]
    assert compacted[-1] == long_messages[-1]
    assert stats["after_tokens"] < stats["before_tokens"]
    assert stats["retained_recent_tokens"] > 0

    gateway.settings.auto_compact = True
    gateway.settings.threshold = 80
    gateway.settings.context_spec = "100"
    with mock.patch.object(gateway, "exact_message_tokens", return_value=70), \
         mock.patch.object(gateway, "compact_messages",
                           return_value=([{"role": "system", "content": "memory"}],
                                         {"before_tokens": 70, "after_tokens": 10,
                                          "retained_recent_tokens": 0})) as compact:
        compacted, stats = gateway.maybe_compact(
            {"max_tokens": 10, "messages": long_messages}, long_messages
        )
    compact.assert_called_once()
    assert stats["after_tokens"] == 10 and compacted[0]["content"] == "memory"

    # The browser streaming path persists every content chunk before emitting
    # its successful terminal event.
    class FakeResponse:
        def __init__(self):
            self.data = (
                b'data: {"choices":[{"delta":{"content":"first "},'
                b'"finish_reason":null}]}\n\n'
                b'data: {"choices":[{"delta":{"content":"second"},'
                b'"finish_reason":null}]}\n\n'
                b'data: {"choices":[{"delta":{},"finish_reason":"stop"}]}\n\n'
                b'data: [DONE]\n\n'
            )
        def read(self, _size):
            value, self.data = self.data, b""
            return value

    handler = object.__new__(gateway.Handler)
    handler.wfile = io.BytesIO()
    handler.ledger_id = "stream-chat"
    handler.ledger_messages = [{"role": "user", "content": "say two words"}]
    handler.compaction_meta = None
    gateway.supervisor.backend = "ornith"
    with mock.patch.object(gateway.ledger, "save") as saved:
        call, text = handler.relay_pass(FakeResponse(), final=False)
    assert call is None and text == "first second"
    saved.assert_called_once()
    assert saved.call_args.args[2][-1]["content"] == "first second"
    wire = handler.wfile.getvalue().decode()
    assert wire.count('"finish_reason":"stop"') == 1
    assert '"session_saved":true' in wire

print("gateway context/compaction checks: PASS")

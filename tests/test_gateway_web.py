#!/usr/bin/env python3
import importlib.util
import os
from pathlib import Path
import socket
import tempfile
from unittest import mock
import json


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
        "samosa_gateway", Path(__file__).parents[1] / "tools/samosa_gateway.py"
    )
    gateway = importlib.util.module_from_spec(spec)
    assert spec.loader
    spec.loader.exec_module(gateway)

    blocked = [
        "0.0.0.0", "0.1.2.3", "10.0.0.1", "100.64.0.1", "100.127.255.254",
        "127.0.0.1", "127.255.255.254", "169.254.169.254", "172.16.0.1",
        "172.31.255.254", "192.0.0.1", "192.168.0.1", "198.18.0.1",
        "198.19.255.254", "224.0.0.1", "239.255.255.254", "240.0.0.1",
        "255.255.255.255", "::", "::1", "fc00::1", "fdff::1", "fe80::1",
        "::ffff:127.0.0.1", "::ffff:10.0.0.1", "64:ff9b::7f00:1",
        "2002:7f00:1::", "2001:db8::1", "198.51.100.1", "203.0.113.1",
    ]

    def answer(address):
        family = socket.AF_INET6 if ":" in address else socket.AF_INET
        return [(family, socket.SOCK_STREAM, 6, "", (address, 0, 0, 0) if family == socket.AF_INET6 else (address, 0))]

    for address in blocked:
        with mock.patch.object(socket, "getaddrinfo", return_value=answer(address)):
            try:
                gateway.public_address("attacker.test")
            except ValueError:
                pass
            else:
                raise AssertionError(f"accepted blocked address {address}")

    # A rebinding/multi-answer response is rejected if any answer is private.
    mixed = answer("93.184.216.34") + answer("127.0.0.1")
    with mock.patch.object(socket, "getaddrinfo", return_value=mixed):
        try:
            gateway.public_address("rebind.test")
        except ValueError:
            pass
        else:
            raise AssertionError("accepted a public/private rebinding answer")

    with mock.patch.object(socket, "getaddrinfo", return_value=answer("93.184.216.34")):
        assert gateway.public_address("example.test")[0] == "93.184.216.34"

    parser = gateway.DuckDuckGoExtractor()
    parser.feed(
        '<a class="result__a" href="https://duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Freview">'
        'A Review</a><a class="result__snippet">Strong review text.</a>'
    )
    assert parser.results == [{
        "title": "A Review",
        "url": "https://example.com/review",
        "description": "Strong review text.",
    }]

    # --- model-decided tool protocol -------------------------------------

    chat = {
        "messages": [{"role": "user", "content": "find an imax theater near clemson sc"}],
        "stream": True,
    }
    prepared = gateway.prepare_chat_payload(json.dumps(chat).encode())
    system = prepared["messages"][0]
    assert system["role"] == "system"
    assert "current local date" in system["content"]
    assert '"samosa_tool":"web_search"' in system["content"]
    assert '"samosa_tool":"open_url"' in system["content"]
    assert prepared["messages"][1]["content"].startswith("find an imax")
    assert gateway.prepare_chat_payload(b"not json") is None

    assert gateway.classify_reply('{"samosa_tool":"web_search","query":"x"}') == \
        ("tool", {"samosa_tool": "web_search", "query": "x"})
    assert gateway.classify_reply('{"samosa_tool":"web_se')[0] == "wait"
    assert gateway.classify_reply("The answer is 4.")[0] == "text"
    assert gateway.classify_reply('{"answer": 42}')[0] == "text"
    assert gateway.classify_reply('```json\n{"samosa_tool":"open_url","url":"https://a.b"}\n```')[0] == "tool"

    call = {"samosa_tool": "web_search", "query": "x"}
    gateway.supervisor.backend = "ornith"
    follow = gateway.followup_payload(chat, '{"samosa_tool":"web_search","query":"x"}', call, "RESULT", remaining=2)
    assert follow["messages"][-2]["role"] == "assistant"
    assert follow["messages"][-1]["content"].startswith("SAMOSA_TOOL_RESULT web_search\nRESULT")
    assert "2 tool calls left" in follow["messages"][-1]["content"]
    gateway.supervisor.backend = "qwen"
    follow = gateway.followup_payload(chat, "", call, "RESULT", remaining=0)
    assert follow["messages"] == [{"role": "user", "content": "SAMOSA_TOOL_RESULT web_search\nRESULT"
                                   "\n\n(No tool calls remain for this turn; answer the user now.)"}]

    # --- generic search-provider config -----------------------------------

    assert gateway.json_path({"a": {"b": [{"c": 1}]}}, "a.b.0.c") == 1
    assert gateway.json_path({"a": 1}, "a.b") is None

    provider = dict(gateway.SEARCH_PRESETS["brave"], api_key="SECRET")
    url, headers, body = gateway.build_search_request(provider, "imax near clemson")
    assert url == "https://api.search.brave.com/res/v1/web/search?q=imax+near+clemson&count=8"
    assert headers == {"X-Subscription-Token": "SECRET"}
    assert body is None
    assert not gateway.provider_ready(gateway.SEARCH_PRESETS["brave"])
    assert gateway.provider_ready(provider)

    provider = dict(gateway.SEARCH_PRESETS["tavily"], api_key="TK")
    url, headers, body = gateway.build_search_request(provider, "hello world")
    assert json.loads(body) == {"query": "hello world", "max_results": 8}
    assert headers == {"Authorization": "Bearer TK"}

    with mock.patch.object(gateway, "web_config",
                           return_value={"search": {"backend": "brave", "api_key": "LEG"}}):
        name, provider = gateway.resolve_search_provider(gateway.search_settings())
    assert name == "brave" and provider["api_key"] == "LEG"

    with mock.patch.object(gateway, "web_config",
                           return_value={"search": {"provider": "mystery"}}):
        try:
            gateway.resolve_search_provider(gateway.search_settings())
        except ValueError as error:
            assert "presets" in str(error)
        else:
            raise AssertionError("unknown provider without url must raise")

    # A custom provider is executed generically: templated request, dot-path response.
    custom = {
        "url": "https://api.example.com/find?q={query}&k={api_key}",
        "api_key": "K",
        "results": "data.hits",
        "fields": {"title": "name", "url": "link", "description": "summary"},
    }
    reply = json.dumps({"data": {"hits": [
        {"name": "Hit", "link": "https://example.com/1", "summary": "S"},
        {"name": "No URL, dropped", "summary": "S2"},
    ]}}).encode()
    with mock.patch.object(gateway, "fetch_public", return_value=("u", "application/json", reply)) as fetched:
        rows = gateway.run_search_provider(custom, "a b")
    assert fetched.call_args.args[0] == "https://api.example.com/find?q=a+b&k=K"
    assert rows == [{"title": "Hit", "url": "https://example.com/1", "description": "S"}]

print("gateway web/search checks: PASS (32 SSRF cases + tool protocol + search providers)")

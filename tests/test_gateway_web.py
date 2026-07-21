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

    with mock.patch.object(gateway, "robots_allowed", return_value=False):
        try:
            gateway.fetch_public("https://example.com/jobs")
        except PermissionError as error:
            assert "robots.txt" in str(error)
        else:
            raise AssertionError("robots-disallowed URL was fetched")

    robots_body = b"User-agent: *\nDisallow: /private\nAllow: /\n"
    with mock.patch.object(gateway, "fetch_public", return_value=("https://example.com/robots.txt", "text/plain", robots_body)):
        assert gateway.robots_allowed("https://example.com/jobs")
        assert not gateway.robots_allowed("https://example.com/private/listing")

    gateway.MIN_PUBLIC_FETCH_INTERVAL = 2.0
    gateway.PUBLIC_FETCH_LAST_BY_HOST.clear()
    monotonic_values = iter([10.0, 10.5, 12.5])
    with mock.patch.object(gateway.time, "monotonic", side_effect=lambda: next(monotonic_values)):
        with mock.patch.object(gateway.time, "sleep") as slept:
            gateway.wait_public_fetch_turn("https://example.com/a")
            gateway.wait_public_fetch_turn("https://example.com/b")
    slept.assert_called_once_with(1.5)
    gateway.MIN_PUBLIC_FETCH_INTERVAL = 0

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

    ps_output = f"""
      101 /usr/bin/python {gateway.HOME}/current/bin/samosa-gateway
      102 {gateway.HOME}/backends/prism-llama.cpp/build/bin/llama-server -m model.gguf --port 8643
      103 /usr/bin/python {gateway.HOME}/current/bin/samosa_jobs.py run job.json
      104 /usr/bin/python unrelated.py
    """
    with mock.patch.object(gateway.subprocess, "check_output", return_value=ps_output):
        assert gateway.related_samosa_pids(exclude={101}) == [102, 103]

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

    # --- scheduled public URL job inputs ---------------------------------

    jobs_root = root / "jobs"
    os.environ["SAMOSA_JOBS_DIR"] = str(jobs_root)
    page_v1 = {
        "url": "https://example.com/jobs",
        "title": "Example Jobs",
        "text": "Role A\nRole B",
        "truncated": False,
    }
    page_v2 = dict(page_v1, text="Role A\nRole C")
    with mock.patch.object(gateway, "readable_page", return_value=page_v1) as readable:
        first = gateway.update_job_public_inputs("public-job", ["https://example.com/jobs"])
        second = gateway.update_job_public_inputs("public-job", ["https://example.com/jobs"])
    assert readable.call_count == 2
    assert first["changed"] == 1
    assert first["changed_items"][0]["status"] == "new"
    assert second["changed"] == 0
    assert second["records"][0]["status"] == "unchanged"
    assert Path(first["changed_items"][0]["text_path"]).is_file()
    assert (jobs_root / "public-job" / "public" / "state.json").is_file()

    with mock.patch.object(gateway, "readable_page", return_value=page_v2):
        changed = gateway.update_job_public_inputs("public-job", ["https://example.com/jobs"])
    assert changed["changed"] == 1
    assert changed["changed_items"][0]["status"] == "changed"
    assert "Role C" in Path(changed["changed_items"][0]["text_path"]).read_text()

    resume = root / "resume.txt"
    resume.write_text("Deepa Resume\nPython\n")
    page_v3 = {
        "url": "https://example.com/new-role",
        "title": "New Role",
        "text": "Python job posting",
        "truncated": False,
    }
    with mock.patch.object(gateway, "readable_page", return_value=page_v3):
        workflow = gateway.prepare_resume_public_workflow(
            "resume-job", str(resume), ["https://example.com/new-role"])
        unchanged_workflow = gateway.prepare_resume_public_workflow(
            "resume-job", str(resume), ["https://example.com/new-role"])
    assert workflow["changed"] == 1
    assert len(workflow["pairs"]) == 1
    assert Path(workflow["pairs"][0]["resume_path"]).read_text() == "Deepa Resume\nPython\n"
    assert "Python job posting" in Path(workflow["pairs"][0]["posting_text_path"]).read_text()
    assert unchanged_workflow["changed"] == 0
    assert unchanged_workflow["pairs"] == []

print("gateway web/search checks: PASS (32 SSRF cases + tool protocol + search providers)")

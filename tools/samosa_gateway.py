#!/usr/bin/env python3
"""Local Samosa app gateway and model supervisor.

The gateway owns exactly one inference backend at a time.  The browser remains
connected to this process while Qwen or Bonsai is stopped and replaced.
"""

from __future__ import annotations

import http.client
from html.parser import HTMLParser
import ipaddress
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import quote_plus, urljoin, urlsplit
import urllib.request
import urllib.robotparser
import xml.etree.ElementTree as ET

# The jobs layer lives beside the gateway (tools/ in source, bin/ when
# installed). Put our own directory on the path so it — and the tools/jobs_fs
# it pulls in — import from either location.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import samosa_jobs
import samosa_tools


HOST = "127.0.0.1"
PUBLIC_PORT = int(os.environ.get("SAMOSA_PORT", "8642"))
BACKEND_PORT = int(os.environ.get("SAMOSA_BACKEND_PORT", str(PUBLIC_PORT + 1)))
HOME = Path(os.environ.get("SAMOSA_HOME", Path.home() / ".samosa"))
APP_HTML = Path(os.environ["SAMOSA_APP_HTML"])
APP_LOGO = Path(os.environ["SAMOSA_APP_LOGO"])
QWEN_ENGINE = Path(os.environ["SAMOSA_QWEN_ENGINE"])
QWEN_MODEL = Path(os.environ["SAMOSA_QWEN_MODEL"])
TOKENIZER = Path(os.environ["SAMOSA_TOKENIZER"])
BONSAI_SERVER = Path(os.environ.get("SAMOSA_BONSAI_SERVER", HOME / "backends/prism-llama.cpp/build/bin/llama-server"))
BONSAI_MODEL = Path(os.environ.get("SAMOSA_BONSAI_MODEL", HOME / "models/bonsai-27b-1bit/Bonsai-27B-Q1_0.gguf"))
ORNITH_MODEL = Path(os.environ.get("SAMOSA_ORNITH_MODEL", HOME / "models/ornith-9b/Ornith-1.0-9B-Q4_K_M.gguf"))
SELECTION_FILE = HOME / "model-backend"
BACKEND_LOG = HOME / "backend.log"
CONFIG_FILE = HOME / "config.json"
MAX_WEB_BYTES = 5 * 1024 * 1024
MAX_WEB_TEXT = 120_000
MAX_TOOL_ROUNDS = 4
MIN_PUBLIC_FETCH_INTERVAL = float(os.environ.get("SAMOSA_WEB_MIN_INTERVAL", "1.0"))
PUBLIC_FETCH_USER_AGENT = "SamosaChat/1.0 (+local user-initiated fetch)"
BLOCKED_NETWORKS = tuple(ipaddress.ip_network(value) for value in (
    "0.0.0.0/8", "10.0.0.0/8", "100.64.0.0/10", "127.0.0.0/8",
    "169.254.0.0/16", "172.16.0.0/12", "192.0.0.0/24", "192.168.0.0/16",
    "198.18.0.0/15", "224.0.0.0/4", "240.0.0.0/4",
    "::/128", "::1/128", "fc00::/7", "fe80::/10",
    "::ffff:0:0/96", "64:ff9b::/96", "2002::/16",
))
PUBLIC_FETCH_LOCK = threading.Lock()
PUBLIC_FETCH_LAST_BY_HOST: dict[str, float] = {}

BACKENDS = {
    "qwen": {
        "label": "Qwen3.6 35B A3B",
        "model": "qwen3.6-35b-a3b",
        "supports_images": True,
    },
    "bonsai": {
        "label": "Bonsai 27B 1-bit",
        "model": "bonsai-27b-1bit",
        "supports_images": False,
    },
    "ornith": {
        "label": "Ornith 9B",
        "model": "ornith-1.0-9b",
        "supports_images": False,
    },
}

# GGUF backends all run through the same Prism llama-server binary.
GGUF_MODELS = {
    "bonsai": BONSAI_MODEL,
    "ornith": ORNITH_MODEL,
}


class Supervisor:
    def __init__(self) -> None:
        self.lock = threading.RLock()
        self.process: subprocess.Popen[bytes] | None = None
        self.backend = self._saved_backend()
        self.started_at = 0.0
        self.generating = False
        self.upstream: http.client.HTTPConnection | None = None
        self.upstream_response: http.client.HTTPResponse | None = None
        self.stopping = False

    def _saved_backend(self) -> str:
        try:
            name = SELECTION_FILE.read_text().strip()
            if name in BACKENDS and self.available(name):
                return name
        except OSError:
            pass
        return "bonsai" if self.available("bonsai") else "qwen"

    @staticmethod
    def available(name: str) -> bool:
        if name == "qwen":
            return QWEN_ENGINE.is_file() and (QWEN_MODEL / "experts.bin").is_file()
        return BONSAI_SERVER.is_file() and GGUF_MODELS[name].is_file()

    def command(self, name: str) -> tuple[list[str], dict[str, str]]:
        env = os.environ.copy()
        if name == "qwen":
            env.update({
                "SNAP": str(QWEN_MODEL),
                "TOKENIZER": str(TOKENIZER),
                "SAMOSA_CHATS_DIR": str(HOME / "chats"),
            })
            return [
                str(QWEN_ENGINE), "--serve", "--port", str(BACKEND_PORT),
                "--tokenizer", str(TOKENIZER),
            ], env
        return [
            str(BONSAI_SERVER), "-m", str(GGUF_MODELS[name]), "-ngl", "99",
            "-c", "8192", "-np", "1", "--cache-ram", "0",
            "--host", HOST, "--port", str(BACKEND_PORT), "--no-ui",
            "--alias", BACKENDS[name]["model"],
        ], env

    def start(self) -> None:
        with self.lock:
            if self.process and self.process.poll() is None:
                return
            if not self.available(self.backend):
                raise RuntimeError(f"{self.backend} backend is not installed")
            HOME.mkdir(parents=True, exist_ok=True)
            (HOME / "chats").mkdir(parents=True, exist_ok=True)
            command, env = self.command(self.backend)
            log = open(BACKEND_LOG, "ab", buffering=0)
            log.write(f"\n--- starting {self.backend} at {time.ctime()} ---\n".encode())
            self.process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
            log.close()
            self.started_at = time.time()

    def stop(self) -> None:
        with self.lock:
            process = self.process
            upstream = self.upstream
            response = self.upstream_response
            self.process = None
            self.upstream = None
            self.upstream_response = None
            self.generating = False
        if response:
            try:
                response.close()
            except OSError:
                pass
        if upstream:
            try:
                upstream.close()
            except OSError:
                pass
        if process and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)

    def select(self, name: str) -> None:
        if name not in BACKENDS:
            raise ValueError("unknown backend")
        if not self.available(name):
            raise FileNotFoundError(f"{BACKENDS[name]['label']} is not installed")
        with self.lock:
            if self.generating:
                raise RuntimeError("stop the current response before switching models")
            if name == self.backend and self.process and self.process.poll() is None:
                return
        self.stop()
        with self.lock:
            self.backend = name
            SELECTION_FILE.write_text(name + "\n")
        self.start()

    def cancel(self) -> bool:
        with self.lock:
            upstream = self.upstream
            response = self.upstream_response
            backend = self.backend
            generating = self.generating
        if not generating:
            return False
        if backend == "qwen":
            try:
                conn = http.client.HTTPConnection(HOST, BACKEND_PORT, timeout=3)
                conn.request("POST", "/v1/cancel", body=b"", headers={"Content-Length": "0"})
                reply = conn.getresponse()
                reply.read()
                conn.close()
                return reply.status == 200
            except (OSError, http.client.HTTPException):
                pass
        if response:
            try:
                response.close()
            except OSError:
                pass
        try:
            if upstream and upstream.sock:
                upstream.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            upstream.close()
        except OSError:
            pass
        # llama.cpp normally cancels when its response reader disappears, but
        # some HTTPConnection states detach the response's socket. Restarting
        # the small resident llama-server backend is the deterministic fallback.
        if backend != "qwen":
            threading.Thread(target=self._restart_cancelled_backend, daemon=True).start()
        return True

    def _restart_cancelled_backend(self) -> None:
        self.stop()
        if not self.stopping:
            try:
                self.start()
            except (OSError, RuntimeError):
                pass

    def ready(self) -> bool:
        with self.lock:
            process = self.process
            backend = self.backend
        if not process or process.poll() is not None:
            return False
        path = "/healthz" if backend == "qwen" else "/health"
        try:
            conn = http.client.HTTPConnection(HOST, BACKEND_PORT, timeout=0.5)
            conn.request("GET", path)
            response = conn.getresponse()
            with supervisor.lock:
                supervisor.upstream_response = response
            response.read()
            conn.close()
            return response.status == 200
        except OSError:
            return False

    def status(self) -> dict:
        with self.lock:
            name = self.backend
            process = self.process
            generating = self.generating
        return {
            "gateway": True,
            "backend": name,
            "label": BACKENDS[name]["label"],
            "model": BACKENDS[name]["model"],
            "supports_images": BACKENDS[name]["supports_images"],
            "ready": self.ready(),
            "loading": bool(process and process.poll() is None and not self.ready()),
            "generating": generating,
            "pid": process.pid if process and process.poll() is None else None,
        }

    def listing(self) -> dict:
        return {
            "active": self.backend,
            "backends": [
                {**{"id": name}, **details, "available": self.available(name)}
                for name, details in BACKENDS.items()
            ],
        }


supervisor = Supervisor()


class GatewayServer(ThreadingHTTPServer):
    allow_reuse_address = True


class TextExtractor(HTMLParser):
    SKIP = {"script", "style", "svg", "noscript", "template"}

    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.skip = 0
        self.title = ""
        self.in_title = False
        self.parts: list[str] = []

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        tag = tag.lower()
        if tag in self.SKIP:
            self.skip += 1
        elif tag == "title":
            self.in_title = True
        elif tag in {"p", "br", "li", "article", "section", "h1", "h2", "h3", "tr"}:
            self.parts.append("\n")

    def handle_endtag(self, tag: str) -> None:
        tag = tag.lower()
        if tag in self.SKIP and self.skip:
            self.skip -= 1
        elif tag == "title":
            self.in_title = False

    def handle_data(self, data: str) -> None:
        if self.skip:
            return
        clean = re.sub(r"\s+", " ", data).strip()
        if clean:
            if self.in_title:
                self.title = (self.title + " " + clean).strip()
            self.parts.append(clean)

    def text(self) -> str:
        value = " ".join(self.parts)
        value = re.sub(r" *\n *", "\n", value)
        value = re.sub(r"\n{3,}", "\n\n", value)
        value = re.sub(r"[ \t]{2,}", " ", value)
        return value.strip()


class DuckDuckGoExtractor(HTMLParser):
    def __init__(self) -> None:
        super().__init__(convert_charrefs=True)
        self.results: list[dict] = []
        self.current: dict | None = None
        self.in_title = False
        self.in_snippet = False

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        values = dict(attrs)
        classes = set((values.get("class") or "").split())
        if tag == "a" and "result__a" in classes:
            raw_url = values.get("href") or ""
            parsed = urlsplit(raw_url)
            target = raw_url
            if parsed.hostname and parsed.hostname.endswith("duckduckgo.com"):
                from urllib.parse import parse_qs
                target = parse_qs(parsed.query).get("uddg", [raw_url])[0]
            self.current = {"title": "", "url": target, "description": ""}
            self.in_title = True
        elif self.current is not None and ("result__snippet" in classes or "result-snippet" in classes):
            self.in_snippet = True

    def handle_endtag(self, tag: str) -> None:
        if tag == "a" and self.in_title and self.current is not None:
            self.in_title = False
            if self.current["url"].startswith(("http://", "https://")):
                self.results.append(self.current)
        if self.in_snippet and tag in {"a", "div", "td"}:
            self.in_snippet = False
            self.current = None

    def handle_data(self, data: str) -> None:
        clean = re.sub(r"\s+", " ", data).strip()
        if not clean or self.current is None:
            return
        if self.in_title:
            self.current["title"] += (" " if self.current["title"] else "") + clean
        elif self.in_snippet:
            self.current["description"] += (" " if self.current["description"] else "") + clean


def web_config() -> dict:
    try:
        data = json.loads(CONFIG_FILE.read_text())
        return data if isinstance(data, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}


def offline() -> bool:
    return os.environ.get("SAMOSA_OFFLINE", "").lower() in {"1", "true", "yes"} or bool(web_config().get("offline"))


def public_address(host: str) -> tuple[str, int]:
    try:
        port = 0
        results = socket.getaddrinfo(host, None, type=socket.SOCK_STREAM)
    except socket.gaierror as error:
        raise ValueError(f"could not resolve {host}: {error}") from error
    addresses = []
    for result in results:
        value = result[4][0]
        address = ipaddress.ip_address(value)
        if not address.is_global or any(address in network for network in BLOCKED_NETWORKS if network.version == address.version):
            raise ValueError(f"blocked non-public address for {host}")
        # Transition mechanisms can reach an IPv4 target not represented by
        # is_global on the outer IPv6 address.
        if address.version == 6 and (
            address.ipv4_mapped is not None
            or address in ipaddress.ip_network("64:ff9b::/96")
            or address in ipaddress.ip_network("2002::/16")
        ):
            raise ValueError(f"blocked transition address for {host}")
        addresses.append(value)
    if not addresses:
        raise ValueError(f"{host} has no usable address")
    return addresses[0], port


def public_fetch_host_key(url: str) -> str:
    parsed = urlsplit(url)
    port = parsed.port or (443 if parsed.scheme == "https" else 80)
    return f"{parsed.hostname or ''}:{port}"


def wait_public_fetch_turn(url: str) -> None:
    if MIN_PUBLIC_FETCH_INTERVAL <= 0:
        return
    key = public_fetch_host_key(url)
    with PUBLIC_FETCH_LOCK:
        now = time.monotonic()
        wait = PUBLIC_FETCH_LAST_BY_HOST.get(key, 0.0) + MIN_PUBLIC_FETCH_INTERVAL - now
        if wait > 0:
            time.sleep(wait)
            now = time.monotonic()
        PUBLIC_FETCH_LAST_BY_HOST[key] = now


def robots_url_for(url: str) -> str:
    parsed = urlsplit(url)
    if parsed.scheme not in {"http", "https"} or not parsed.hostname:
        raise ValueError("only public http:// and https:// URLs are allowed")
    port = f":{parsed.port}" if parsed.port is not None else ""
    return f"{parsed.scheme}://{parsed.hostname}{port}/robots.txt"


def robots_allowed(url: str, user_agent: str = PUBLIC_FETCH_USER_AGENT) -> bool:
    robots_url = robots_url_for(url)
    try:
        final_url, content_type, data = fetch_public(
            robots_url,
            user_agent=user_agent,
            enforce_robots=False,
        )
    except Exception:
        return True
    if content_type not in {"text/plain", "text/html"}:
        return True
    parser = urllib.robotparser.RobotFileParser()
    parser.set_url(final_url)
    parser.parse(data.decode("utf-8", errors="replace").splitlines())
    return parser.can_fetch(user_agent, url)


def fetch_public(
    url: str,
    accept_json: bool = False,
    extra_headers: dict[str, str] | None = None,
    user_agent: str = PUBLIC_FETCH_USER_AGENT,
    json_body: bytes | None = None,
    enforce_robots: bool = True,
) -> tuple[str, str, bytes]:
    if offline():
        raise PermissionError("Internet access is disabled")
    current = url.strip()
    for _hop in range(6):
        parsed = urlsplit(current)
        if parsed.scheme not in {"http", "https"} or not parsed.hostname or parsed.username or parsed.password:
            raise ValueError("only public http:// and https:// URLs are allowed")
        if parsed.port is not None and parsed.port not in {80, 443}:
            raise ValueError("non-standard URL ports are blocked")
        if enforce_robots and not robots_allowed(current, user_agent=user_agent):
            raise PermissionError("robots.txt disallows this URL")
        wait_public_fetch_turn(current)
        ip, _ = public_address(parsed.hostname)
        port = parsed.port or (443 if parsed.scheme == "https" else 80)
        with tempfile.TemporaryDirectory(prefix="samosa-web-") as temp:
            headers = Path(temp) / "headers"
            body = Path(temp) / "body"
            command = [
                "curl", "--silent", "--show-error", "--fail-with-body",
                "--proto", "=http,https", "--max-redirs", "0", "--max-time", "20",
                "--connect-timeout", "5", "--max-filesize", str(MAX_WEB_BYTES),
                "--resolve", f"{parsed.hostname}:{port}:{ip}",
                "-A", user_agent,
                "-D", str(headers), "-o", str(body), "-w", "%{http_code}",
            ]
            if accept_json:
                command += ["-H", "Accept: application/json"]
            if json_body is not None:
                request_body = Path(temp) / "request"
                request_body.write_bytes(json_body)
                command += ["-X", "POST", "-H", "Content-Type: application/json",
                            "--data-binary", "@" + str(request_body)]
            curl_config = ""
            for key, value in (extra_headers or {}).items():
                if any(c in key + value for c in "\r\n\0"):
                    raise ValueError("invalid outbound header")
                escaped = (key + ": " + value).replace("\\", "\\\\").replace('"', '\\"')
                curl_config += f'header = "{escaped}"\n'
            if curl_config:
                command += ["--config", "-"]
            command.append(current)
            result = subprocess.run(command, input=curl_config or None, capture_output=True, text=True, timeout=22)
            status_text = result.stdout[-3:]
            status = int(status_text) if status_text.isdigit() else 0
            raw_headers = headers.read_text(errors="replace") if headers.exists() else ""
            location = ""
            content_type = ""
            for line in raw_headers.splitlines():
                key, sep, value = line.partition(":")
                if sep and key.lower() == "location":
                    location = value.strip()
                if sep and key.lower() == "content-type":
                    content_type = value.split(";", 1)[0].strip().lower()
            if status in {301, 302, 303, 307, 308} and location:
                current = urljoin(current, location)
                continue
            if result.returncode or not 200 <= status < 300:
                raise ValueError(f"fetch failed with HTTP {status or 'error'}")
            data = body.read_bytes()
            if len(data) > MAX_WEB_BYTES:
                raise ValueError("page exceeds the 5 MB limit")
            allowed = {"text/html", "text/plain", "application/json", "text/xml", "application/xml", "application/rss+xml"}
            if content_type not in allowed:
                raise ValueError(f"unsupported content type: {content_type or 'unknown'}")
            return current, content_type, data
    raise ValueError("too many redirects")


def readable_page(url: str) -> dict:
    final_url, content_type, data = fetch_public(url)
    decoded = data.decode("utf-8", errors="replace")
    if content_type == "text/html":
        parser = TextExtractor()
        parser.feed(decoded)
        text = parser.text()
        title = parser.title or urlsplit(final_url).hostname or final_url
        script_count = decoded.lower().count("<script")
        if len(text) < 300 and script_count >= 3:
            raise ValueError("this page appears to require JavaScript and could not be read")
    else:
        text = decoded
        title = urlsplit(final_url).path.rsplit("/", 1)[-1] or final_url
    if not text.strip():
        raise ValueError("the page did not contain readable text")
    truncated = len(text) > MAX_WEB_TEXT
    return {"url": final_url, "title": title[:300], "text": text[:MAX_WEB_TEXT], "truncated": truncated}


# Declarative descriptors for well-known search services. A user connects one
# by naming it in config.json and supplying only its credentials; any other
# HTTP JSON search API can be described with the same fields under
# search.providers without touching this file:
#   url      — request URL template; {query} is URL-encoded, other {name}
#              placeholders resolve from the provider's own config values
#   headers  — outbound headers, same placeholder substitution
#   body     — JSON object for a POST request (string values substituted)
#   results  — dot-path to the result array in the JSON response
#   fields   — dot-paths for title/url/description within one result
SEARCH_PRESETS = {
    "brave": {
        "url": "https://api.search.brave.com/res/v1/web/search?q={query}&count=8",
        "headers": {"X-Subscription-Token": "{api_key}"},
        "results": "web.results",
        "fields": {"title": "title", "url": "url", "description": "description"},
    },
    "tavily": {
        "url": "https://api.tavily.com/search",
        "headers": {"Authorization": "Bearer {api_key}"},
        "body": {"query": "{query}", "max_results": 8},
        "results": "results",
        "fields": {"title": "title", "url": "url", "description": "content"},
    },
    "serpapi": {
        "url": "https://serpapi.com/search.json?engine=google&q={query}&api_key={api_key}",
        "results": "organic_results",
        "fields": {"title": "title", "url": "link", "description": "snippet"},
    },
    "google": {
        "url": "https://www.googleapis.com/customsearch/v1?key={api_key}&cx={cx}&q={query}",
        "results": "items",
        "fields": {"title": "title", "url": "link", "description": "snippet"},
    },
    "searxng": {
        "url": "{base_url}/search?q={query}&format=json",
        "results": "results",
        "fields": {"title": "title", "url": "url", "description": "content"},
    },
}


def json_path(value: object, path: str) -> object:
    for part in path.split("."):
        if isinstance(value, dict):
            value = value.get(part)
        elif isinstance(value, list) and part.isdigit() and int(part) < len(value):
            value = value[int(part)]
        else:
            return None
    return value


def search_settings() -> dict:
    config = web_config()
    search = config.get("search", {}) if isinstance(config.get("search"), dict) else {}
    # Legacy config compatibility: {"backend": "brave", "api_key": ...} and
    # {"backend": "searxng", "url": ...} predate the provider system.
    if "provider" not in search and search.get("backend"):
        search = dict(search)
        search["provider"] = search["backend"]
        overrides = search.setdefault("providers", {})
        if isinstance(overrides, dict):
            legacy = overrides.setdefault(str(search["backend"]), {})
            if isinstance(legacy, dict):
                if search.get("api_key"):
                    legacy.setdefault("api_key", search["api_key"])
                if search.get("url"):
                    legacy.setdefault("base_url", str(search["url"]).rstrip("/"))
    return search


def resolve_search_provider(search: dict) -> tuple[str, dict | None]:
    name = str(search.get("provider", "auto"))
    if name in {"auto", "duckduckgo"}:
        return name, None
    provider = dict(SEARCH_PRESETS.get(name, {}))
    overrides = search.get("providers", {})
    if isinstance(overrides, dict) and isinstance(overrides.get(name), dict):
        provider.update(overrides[name])
    if not provider.get("url"):
        raise ValueError(
            f"search provider {name!r} is not a preset and has no 'url' in config.json; "
            f"presets: {', '.join(sorted(SEARCH_PRESETS))}"
        )
    return name, provider


def provider_ready(provider: dict) -> bool:
    """True when every placeholder in the provider's templates has a config value."""
    try:
        build_search_request(provider, "connectivity-check")
        return True
    except ValueError:
        return False


def build_search_request(provider: dict, query: str) -> tuple[str, dict[str, str], bytes | None]:
    values = {key: str(value) for key, value in provider.items()
              if isinstance(value, (str, int, float))}

    def fill(template: str, urlencode_query: bool) -> str:
        def replace(match: re.Match) -> str:
            key = match.group(1)
            if key == "query":
                return quote_plus(query) if urlencode_query else query
            value = values.get(key)
            if value is None:
                raise ValueError(f"search provider needs {key!r} set in config.json")
            return value
        return re.sub(r"\{(\w+)\}", replace, template)

    url = fill(str(provider["url"]), urlencode_query=True)
    headers = {key: fill(str(value), False)
               for key, value in (provider.get("headers") or {}).items()}
    body = provider.get("body")
    json_body = None
    if body is not None:
        json_body = json.dumps({
            key: (fill(value, False) if isinstance(value, str) else value)
            for key, value in dict(body).items()
        }, separators=(",", ":")).encode()
    return url, headers, json_body


def run_search_provider(provider: dict, query: str) -> list[dict]:
    url, headers, json_body = build_search_request(provider, query)
    _, _, data = fetch_public(url, accept_json=True, extra_headers=headers, json_body=json_body)
    payload = json.loads(data)
    raw_rows = json_path(payload, str(provider.get("results", "results")))
    fields = provider.get("fields") or {}
    limit = int(provider.get("max_results", 8))
    rows = []
    if isinstance(raw_rows, list):
        for item in raw_rows[:limit]:
            if not isinstance(item, dict):
                continue
            row = {
                "title": str(json_path(item, str(fields.get("title", "title"))) or ""),
                "url": str(json_path(item, str(fields.get("url", "url"))) or ""),
                "description": str(json_path(item, str(fields.get("description", "description"))) or ""),
            }
            if row["url"]:
                rows.append(row)
    if not rows:
        raise ValueError("the search provider returned no results")
    return rows


def keyless_search(query: str) -> list[dict]:
    try:
        _, _, data = fetch_public(
            "https://html.duckduckgo.com/html/?q=" + quote_plus(query),
            user_agent="Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 Safari/537.36",
        )
        parser = DuckDuckGoExtractor()
        parser.feed(data.decode("utf-8", errors="replace"))
        if parser.results:
            return parser.results[:5]
    except (ValueError, OSError, subprocess.SubprocessError):
        pass
    # DuckDuckGo's HTML endpoint sometimes presents a human challenge.
    # Bing's small RSS representation is the no-key fallback; it contains
    # normal ranked links and no JavaScript.
    _, _, data = fetch_public(
        "https://www.bing.com/search?q=" + quote_plus(query) + "&format=rss",
        accept_json=False,
    )
    root = ET.fromstring(data)
    rows = []
    for item in root.findall(".//item")[:5]:
        rows.append({
            "title": item.findtext("title", ""),
            "url": item.findtext("link", ""),
            "description": re.sub(r"<[^>]+>", " ", item.findtext("description", "")),
        })
    if not rows:
        raise ValueError("No-key search providers returned no readable results")
    return rows


def search_web(query: str) -> list[dict]:
    if offline():
        raise PermissionError("Internet access is disabled")
    query = query.strip()
    if not query or len(query) > 500:
        raise ValueError("search query must be 1–500 characters")
    search = search_settings()
    name, provider = resolve_search_provider(search)
    if provider is None:
        return keyless_search(query)
    try:
        return run_search_provider(provider, query)
    except (ValueError, OSError, json.JSONDecodeError, subprocess.SubprocessError) as error:
        if not search.get("fallback", True):
            raise
        sys.stderr.write(f"[gateway] search provider {name!r} failed ({error}); using keyless search\n")
        return keyless_search(query)


def ability_prompt(locality: str) -> str:
    location = f"\n\nThe user's approximate location is {locality}." if locality else ""
    if offline():
        return location
    return (
        "\n\nYou have real abilities this app runs for you. To use one, reply with ONLY a single "
        "line of JSON — no other words, no code fences:\n"
        '{"samosa_tool":"web_search","query":"..."} — search the public web\n'
        '{"samosa_tool":"open_url","url":"https://..."} — read one public web page\n'
        "The app will run the tool and reply with a message beginning SAMOSA_TOOL_RESULT; use "
        f"that output to answer the user. You may use at most {MAX_TOOL_ROUNDS} tool calls per "
        "user message, so make each one count and answer as soon as you have enough. Do not "
        "repeat similar searches — if results name a promising page, open_url it instead. "
        "Use a tool whenever the question involves current "
        "events, news, weather, prices, schedules, businesses, opening hours, or nearby places, "
        "or when you are unsure your knowledge is current. For near/nearby questions, put the "
        "place name in the search query (e.g. 'imax theaters near clemson sc') and open a "
        "result page to confirm details are current. Never invent facts a tool did not return, "
        "cite source URLs from tool output, and if a tool fails, say so plainly instead of "
        "guessing."
        + location
    )


# classify_reply is generic tool-call-protocol parsing with no gateway-specific
# behavior, so chat and jobs share exactly one implementation from the Tool
# layer rather than two copies that could quietly drift apart. Bound to a
# module-level name (not just used inline) so tests can still call
# gateway.classify_reply(...) directly.
classify_reply = samosa_tools.classify_reply


def _web_search_tool_text(query: str) -> str:
    """web_search's result formatting for chat: top hits, with the first two
    pages opportunistically fetched for extra context. Registered as a Tool
    layer tool below; kept here (not in samosa_tools) because it calls
    search_web/readable_page, whose network/SSRF/provider config is
    gateway-owned."""
    rows = search_web(query)
    sections = []
    for index, row in enumerate(rows[:5]):
        lead = ""
        if index < 2:
            try:
                lead = "\n" + readable_page(row["url"])["text"][:600]
            except (ValueError, PermissionError, OSError, subprocess.SubprocessError):
                pass
        sections.append(f"{row['title']}\n{row['url']}\n{row['description']}{lead}".strip())
    return "\n\n---\n\n".join(sections) or "the search returned no results"


def _open_url_tool_text(url: str) -> str:
    page = readable_page(url)
    return f"{page['title']}\n{page['url']}\n\n{page['text'][:4000]}"


samosa_tools.register_web_tools(_web_search_tool_text, _open_url_tool_text)
# Chat's toolset: web only. It has no working folder to jail (unlike a job),
# so its tool calls run with ctx=None — execute_tool() treats that as "no
# mutating tools apply here", which is true: web_search/open_url are read-only.
CHAT_TOOLS = samosa_tools.REGISTRY.subset(["web_search", "open_url"])


def request_location(payload: dict) -> tuple[float, float, str] | None:
    location = payload.get("samosa_location")
    if not isinstance(location, dict):
        return None
    label = str(location.get("label", "")).strip()[:200]
    if "latitude" not in location or "longitude" not in location:
        return (0.0, 0.0, label) if label else None
    try:
        latitude = round(float(location["latitude"]), 3)
        longitude = round(float(location["longitude"]), 3)
    except (TypeError, ValueError):
        return None
    if not -90 <= latitude <= 90 or not -180 <= longitude <= 180:
        return None
    return latitude, longitude, label


def reverse_location(latitude: float, longitude: float) -> str:
    _, _, data = fetch_public(
        "https://nominatim.openstreetmap.org/reverse?format=jsonv2&zoom=12"
        f"&lat={latitude:.3f}&lon={longitude:.3f}",
        accept_json=True,
    )
    payload = json.loads(data)
    address = payload.get("address", {})
    locality = (
        address.get("city") or address.get("town") or address.get("village")
        or address.get("municipality") or address.get("county")
    )
    region = address.get("state") or address.get("region")
    country = address.get("country")
    return ", ".join(dict.fromkeys(str(value) for value in (locality, region, country) if value))


def prepare_chat_payload(body: bytes) -> dict | None:
    """Parse a chat request and inject the date + abilities system prompt.

    Returns None when the body is not a chat payload the gateway understands;
    such requests are proxied through unchanged.
    """
    try:
        payload = json.loads(body)
    except (json.JSONDecodeError, UnicodeDecodeError):
        return None
    if not isinstance(payload, dict) or not isinstance(payload.get("messages"), list):
        return None
    date_context = time.strftime(
        "The host computer's current local date is %A, %B %d, %Y. "
        "Use this date as authoritative; do not claim you lack access to the current date.",
        time.localtime(),
    )
    locality = ""
    location = request_location(payload)
    if location:
        latitude, longitude, supplied_label = location
        locality = supplied_label
        if not locality and (latitude or longitude):
            try:
                locality = reverse_location(latitude, longitude)
            except (ValueError, PermissionError, OSError, json.JSONDecodeError):
                locality = ""
    system_text = date_context + ability_prompt(locality)
    messages = payload["messages"]
    if messages and messages[0].get("role") == "system":
        messages[0]["content"] = str(messages[0].get("content", "")) + "\n\n" + system_text
    else:
        messages.insert(0, {"role": "system", "content": system_text})
    return payload


def followup_payload(payload: dict, assistant_text: str, call: dict,
                     result: str, remaining: int) -> dict:
    """Build the next-round request carrying a tool result back to the model."""
    if remaining <= 0:
        note = "\n\n(No tool calls remain for this turn; answer the user now.)"
    else:
        note = f"\n\n({remaining} tool call{'s' if remaining != 1 else ''} left this turn.)"
    tool_message = f"SAMOSA_TOOL_RESULT {call.get('samosa_tool', '')}\n{result}{note}"
    payload = dict(payload)
    if supervisor.backend == "qwen":
        # The C engine keeps its own history via conversation_id sessions; the
        # tool result is simply the next user turn of the same session.
        payload["messages"] = [{"role": "user", "content": tool_message}]
    else:
        payload["messages"] = list(payload["messages"]) + [
            {"role": "assistant", "content": assistant_text.strip()},
            {"role": "user", "content": tool_message},
        ]
    return payload


def sse_data_events(response: http.client.HTTPResponse):
    """Yield the data payload of each SSE event from a backend stream."""
    pending = b""
    while True:
        try:
            chunk = response.read(16384)
        except (OSError, http.client.HTTPException):
            return
        if not chunk:
            return
        pending += chunk
        while b"\n\n" in pending:
            event, pending = pending.split(b"\n\n", 1)
            for line in event.split(b"\n"):
                if line.startswith(b"data: "):
                    yield line[6:]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "SamosaGateway/1"

    def log_message(self, fmt: str, *args: object) -> None:
        sys.stderr.write("[gateway] " + fmt % args + "\n")

    def body(self) -> bytes:
        length = int(self.headers.get("Content-Length", "0"))
        if length > 4 * 1024 * 1024:
            raise ValueError("request body is too large")
        return self.rfile.read(length)

    def json_response(self, status: int, value: object) -> None:
        data = json.dumps(value, separators=(",", ":")).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def static(self, path: Path, content_type: str) -> None:
        try:
            data = path.read_bytes()
        except OSError:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self) -> None:
        path = urlsplit(self.path).path
        if path in ("/", "/index.html"):
            self.static(APP_HTML, "text/html; charset=utf-8")
        elif path == "/assets/samosa-chat.png":
            self.static(APP_LOGO, "image/png")
        elif path == "/healthz":
            self.json_response(200, supervisor.status())
        elif path == "/v1/backends":
            self.json_response(200, supervisor.listing())
        elif path == "/v1/web/config":
            search = search_settings()
            try:
                name, provider = resolve_search_provider(search)
                configured = provider is None or provider_ready(provider)
            except ValueError:
                name, configured = str(search.get("provider", "auto")), False
            self.json_response(200, {
                "offline": offline(),
                "search_backend": name,
                "search_configured": configured,
            })
        else:
            self.proxy()

    def do_POST(self) -> None:
        path = urlsplit(self.path).path
        if path == "/v1/backends/select":
            try:
                data = json.loads(self.body())
                supervisor.select(data.get("backend", ""))
                self.json_response(202, supervisor.status())
            except (ValueError, FileNotFoundError, RuntimeError) as error:
                self.json_response(409, {"error": {"message": str(error)}})
        elif path == "/v1/web/fetch":
            try:
                data = json.loads(self.body())
                self.json_response(200, readable_page(str(data.get("url", ""))))
            except (ValueError, PermissionError, OSError, subprocess.SubprocessError) as error:
                self.json_response(400, {"error": {"message": str(error)}})
        elif path == "/v1/web/search":
            try:
                data = json.loads(self.body())
                self.json_response(200, {"results": search_web(str(data.get("query", "")))})
            except (ValueError, PermissionError, OSError, json.JSONDecodeError) as error:
                self.json_response(400, {"error": {"message": str(error)}})
        elif path == "/v1/jobs/run":
            self.jobs_stream(self.body(), "run")
        elif path == "/v1/jobs/suggest":
            self.jobs_suggest(self.body())
        elif path == "/v1/jobs/estimate":
            self.jobs_estimate(self.body())
        elif path == "/v1/jobs/review":
            self.jobs_review(self.body())
        elif path == "/v1/jobs/review/correct":
            self.jobs_review_correct(self.body())
        elif path == "/v1/jobs/apply":
            self.jobs_stream(self.body(), "apply")
        elif path == "/v1/jobs/undo":
            self.jobs_stream(self.body(), "undo")
        elif path == "/v1/jobs/answer":
            self.jobs_stream(self.body(), "answer")
        elif path == "/v1/cancel":
            self.json_response(200, {"cancelled": supervisor.cancel()})
        elif path == "/v1/shutdown":
            self.json_response(200, {"stopping": True})
            threading.Thread(target=self.shutdown_all, daemon=True).start()
        else:
            body = self.body()
            if path == "/v1/chat/completions":
                self.chat_proxy(body)
            else:
                self.proxy(body)

    def proxy(self, body: bytes | None = None) -> None:
        status = supervisor.status()
        if not status["ready"]:
            self.json_response(503, {"error": {"message": f"{status['label']} is still loading"}})
            return
        conn = http.client.HTTPConnection(HOST, BACKEND_PORT, timeout=None)
        with supervisor.lock:
            supervisor.upstream = conn
        try:
            headers = {"Host": f"{HOST}:{BACKEND_PORT}", "Connection": "close"}
            if body is not None:
                headers["Content-Type"] = self.headers.get("Content-Type", "application/json")
                headers["Content-Length"] = str(len(body))
            conn.request(self.command, self.path, body=body, headers=headers)
            response = conn.getresponse()
            self.send_response_only(response.status, response.reason)
            for key, value in response.getheaders():
                if key.lower() not in ("connection", "keep-alive", "transfer-encoding"):
                    self.send_header(key, value)
            self.send_header("Connection", "close")
            self.end_headers()
            while True:
                chunk = response.read(16384)
                if not chunk:
                    break
                self.wfile.write(chunk)
                self.wfile.flush()
        except (OSError, http.client.HTTPException, BrokenPipeError):
            if not self.wfile.closed:
                try:
                    self.close_connection = True
                except OSError:
                    pass
        finally:
            conn.close()
            with supervisor.lock:
                if supervisor.upstream is conn:
                    supervisor.upstream = None
                if supervisor.upstream_response is response:
                    supervisor.upstream_response = None

    # --- model-decided tool loop ------------------------------------------

    def chat_proxy(self, body: bytes) -> None:
        status = supervisor.status()
        if not status["ready"]:
            self.json_response(503, {"error": {"message": f"{status['label']} is still loading"}})
            return
        payload = prepare_chat_payload(body)
        if payload is None:
            self.proxy(body)
            return
        with supervisor.lock:
            if supervisor.generating:
                self.json_response(409, {"error": {"message": "another response is already generating"}})
                return
            supervisor.generating = True
        try:
            if payload.get("stream"):
                self.chat_stream(payload)
            else:
                self.chat_once(payload)
        except (OSError, http.client.HTTPException, BrokenPipeError):
            self.close_connection = True
        finally:
            with supervisor.lock:
                supervisor.generating = False

    def backend_chat(self, payload: dict) -> tuple[http.client.HTTPConnection, http.client.HTTPResponse]:
        conn = http.client.HTTPConnection(HOST, BACKEND_PORT, timeout=None)
        with supervisor.lock:
            supervisor.upstream = conn
        data = json.dumps(payload, separators=(",", ":")).encode()
        conn.request("POST", "/v1/chat/completions", body=data, headers={
            "Host": f"{HOST}:{BACKEND_PORT}", "Connection": "close",
            "Content-Type": "application/json", "Content-Length": str(len(data)),
        })
        response = conn.getresponse()
        with supervisor.lock:
            supervisor.upstream_response = response
        return conn, response

    def send_raw_json(self, status: int, data: bytes) -> None:
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    # --- jobs (Models -> Tools -> Jobs) -----------------------------------

    def sse_begin(self) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Accel-Buffering", "no")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True

    def sse_event(self, event: dict) -> None:
        self.wfile.write(b"data: " + json.dumps(event, separators=(",", ":")).encode() + b"\n\n")
        self.wfile.flush()

    def jobs_model_call(self, messages: list) -> str | None:
        """Run one small, non-streaming completion on the active backend.

        Used only to disambiguate a job's intent. Returns None whenever a model
        is not available or busy, so the jobs layer falls back to its
        deterministic (and safe, read-only) default rather than blocking.
        """
        return self._jobs_completion(messages, max_tokens=64)

    def jobs_tool_loop_model_call(self, messages: list) -> str | None:
        """Run one non-streaming completion for a Jobs tool-loop turn.

        Same admission discipline as the 64-token classifier, but with a larger
        answer budget so the model can emit a tool call or final find summary.
        """
        return self._jobs_completion(messages, max_tokens=512)

    def jobs_suggest_model_call(self, messages: list) -> str | None:
        """Run one supervised completion for choosing a shipped job template."""
        return self._jobs_completion(messages, max_tokens=128)

    def _jobs_completion(self, messages: list, max_tokens: int) -> str | None:
        status = supervisor.status()
        if not status.get("ready"):
            return None
        with supervisor.lock:
            if supervisor.generating:
                return None
            supervisor.generating = True
        conn = None
        response = None
        try:
            payload = {"messages": messages, "stream": False, "thinking": "off",
                       "temperature": 0, "max_tokens": max_tokens}
            conn, response = self.backend_chat(payload)
            raw = response.read()
            if response.status != 200:
                return None
            return json.loads(raw)["choices"][0]["message"].get("content") or ""
        except (OSError, http.client.HTTPException, json.JSONDecodeError,
                KeyError, IndexError, TypeError):
            return None
        finally:
            if conn is not None:
                conn.close()
            with supervisor.lock:
                if supervisor.upstream is conn:
                    supervisor.upstream = None
                if supervisor.upstream_response is response:
                    supervisor.upstream_response = None
                supervisor.generating = False

    def jobs_suggest(self, body: bytes) -> None:
        try:
            data = json.loads(body) if body else {}
        except (ValueError, UnicodeDecodeError):
            self.json_response(400, {"error": {"message": "invalid JSON body"}})
            return
        goal = str(data.get("goal", "")).strip()
        folder = str(data.get("folder", "")).strip()
        if not goal or not folder:
            self.json_response(400, {"error": {"message": "goal and folder are required"}})
            return
        result = samosa_jobs.suggest_job(goal, folder,
                                         model_call=self.jobs_suggest_model_call)
        self.json_response(200 if result.get("ok") else 422, result)

    def jobs_estimate(self, body: bytes) -> None:
        try:
            data = json.loads(body) if body else {}
        except (ValueError, UnicodeDecodeError):
            self.json_response(400, {"error": {"message": "invalid JSON body"}})
            return
        job = data.get("job")
        if not isinstance(job, dict):
            self.json_response(400, {"error": {"message": "job is required"}})
            return
        self.json_response(200, samosa_jobs.estimate_job(job))

    def jobs_review(self, body: bytes) -> None:
        try:
            data = json.loads(body) if body else {}
        except (ValueError, UnicodeDecodeError):
            self.json_response(400, {"error": {"message": "invalid JSON body"}})
            return
        job_id = str(data.get("job_id", "")).strip()
        if not job_id:
            self.json_response(400, {"error": {"message": "job_id is required"}})
            return
        self.json_response(200, samosa_jobs.review_items(job_id))

    def jobs_review_correct(self, body: bytes) -> None:
        try:
            data = json.loads(body) if body else {}
        except (ValueError, UnicodeDecodeError):
            self.json_response(400, {"error": {"message": "invalid JSON body"}})
            return
        job_id = str(data.get("job_id", "")).strip()
        if not job_id:
            self.json_response(400, {"error": {"message": "job_id is required"}})
            return
        result = samosa_jobs.correct_review_item(
            job_id,
            data.get("item", {}),
            fields=data.get("fields"),
            mark_done=bool(data.get("mark_done", True)),
        )
        self.json_response(200 if result.get("ok") else 404, result)

    def jobs_stream(self, body: bytes, kind: str) -> None:
        """Stream a job's live events (decode intent, counting, plan, moves)."""
        try:
            data = json.loads(body) if body else {}
        except (ValueError, UnicodeDecodeError):
            self.json_response(400, {"error": {"message": "invalid JSON body"}})
            return

        if kind == "run":
            goal = str(data.get("goal", "")).strip()
            folder = str(data.get("folder", "")).strip()
            mode = data.get("mode", "confirm")
            if mode not in ("confirm", "execute"):
                mode = "confirm"
            if not goal or not folder:
                self.json_response(400, {"error": {"message": "goal and folder are required"}})
                return
            gen = samosa_jobs.run_job(goal, folder, mode=mode, model_call=self.jobs_model_call,
                                      loop_model_call=self.jobs_tool_loop_model_call)
        elif kind in ("apply", "undo"):
            job_id = str(data.get("job_id", "")).strip()
            if not job_id:
                self.json_response(400, {"error": {"message": "job_id is required"}})
                return
            gen = (samosa_jobs.apply_job(job_id) if kind == "apply"
                   else samosa_jobs.undo_job(job_id))
        elif kind == "answer":
            job_id = str(data.get("job_id", "")).strip()
            answer = str(data.get("answer", "")).strip()
            if not job_id or not answer:
                self.json_response(400, {"error": {"message": "job_id and answer are required"}})
                return
            gen = samosa_jobs.answer_job(job_id, answer,
                                         loop_model_call=self.jobs_tool_loop_model_call)
        else:
            self.json_response(404, {"error": {"message": "unknown jobs action"}})
            return

        self.sse_begin()
        try:
            for event in gen:
                self.sse_event(event)
        except (OSError, BrokenPipeError):
            self.close_connection = True
            return
        except Exception as error:  # a job bug becomes a stream error, not a 500
            try:
                self.sse_event({"type": "error", "message": str(error)})
            except OSError:
                pass
        try:
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()
        except OSError:
            self.close_connection = True

    def chat_once(self, payload: dict) -> None:
        for round_index in range(MAX_TOOL_ROUNDS + 1):
            conn, response = self.backend_chat(payload)
            try:
                data = response.read()
            finally:
                conn.close()
            if response.status != 200:
                self.send_raw_json(response.status, data)
                return
            try:
                content = json.loads(data)["choices"][0]["message"].get("content") or ""
            except (json.JSONDecodeError, KeyError, IndexError, TypeError):
                self.send_raw_json(200, data)
                return
            kind, call = classify_reply(content)
            if kind != "tool" or round_index >= MAX_TOOL_ROUNDS:
                self.send_raw_json(200, data)
                return
            result = samosa_tools.execute_tool(call, None, CHAT_TOOLS)
            payload = followup_payload(payload, content, call, result,
                                       remaining=MAX_TOOL_ROUNDS - 1 - round_index)

    def emit_chunk(self, delta: dict, finish: str | None = None, extra: dict | None = None) -> None:
        event = {
            "object": "chat.completion.chunk",
            "choices": [{"index": 0, "delta": delta, "finish_reason": finish}],
        }
        if extra:
            event.update(extra)
        self.wfile.write(b"data: " + json.dumps(event, separators=(",", ":")).encode() + b"\n\n")
        self.wfile.flush()

    def chat_stream(self, payload: dict) -> None:
        started = False
        for round_index in range(MAX_TOOL_ROUNDS + 1):
            conn, response = self.backend_chat(payload)
            try:
                if response.status != 200:
                    data = response.read()
                    if started:
                        message = data.decode("utf-8", "replace")[:300]
                        self.emit_chunk({"content": f"\n[backend error: {message}]"}, finish="stop")
                        self.wfile.write(b"data: [DONE]\n\n")
                        self.wfile.flush()
                    else:
                        self.send_raw_json(response.status, data)
                    return
                if not started:
                    self.send_response(200)
                    self.send_header("Content-Type", "text/event-stream")
                    self.send_header("Cache-Control", "no-store")
                    self.send_header("Connection", "close")
                    self.end_headers()
                    self.close_connection = True
                    started = True
                call, raw_text = self.relay_pass(response, final=round_index >= MAX_TOOL_ROUNDS)
            finally:
                conn.close()
                with supervisor.lock:
                    if supervisor.upstream is conn:
                        supervisor.upstream = None
                    if supervisor.upstream_response is response:
                        supervisor.upstream_response = None
            if call is None:
                self.wfile.write(b"data: [DONE]\n\n")
                self.wfile.flush()
                return
            name = call.get("samosa_tool", "tool")
            arguments = json.dumps(
                {key: value for key, value in call.items() if key != "samosa_tool"},
                ensure_ascii=False,
            )[:160]
            self.emit_chunk({"reasoning_content": f"\n[Samosa runs {name} {arguments}]\n"})
            result = samosa_tools.execute_tool(call, None, CHAT_TOOLS)
            self.emit_chunk({"reasoning_content": f"[{name} returned {len(result)} characters; reading]\n"})
            payload = followup_payload(payload, raw_text, call, result,
                                       remaining=MAX_TOOL_ROUNDS - 1 - round_index)

    def relay_pass(self, response: http.client.HTTPResponse,
                   final: bool) -> tuple[dict | None, str]:
        """Stream one backend pass; hold content back until it is clearly not a tool call.

        Returns (tool_call, accumulated_text); tool_call is None when the pass
        streamed an ordinary answer to the client.
        """
        mode = "undecided"
        buffer = ""
        held_finish: list[tuple[str | None, dict]] = []
        tool_call = None
        for data in sse_data_events(response):
            if data.strip() == b"[DONE]":
                break
            try:
                event = json.loads(data)
            except json.JSONDecodeError:
                continue
            choice = (event.get("choices") or [{}])[0]
            delta = choice.get("delta") or {}
            reasoning = delta.get("reasoning") or delta.get("reasoning_content")
            content = delta.get("content") or ""
            finish = choice.get("finish_reason")
            if reasoning and mode != "tool":
                self.emit_chunk({"reasoning_content": reasoning})
            if content:
                if mode == "stream":
                    self.emit_chunk({"content": content})
                elif mode == "undecided":
                    buffer += content
                    kind, call = classify_reply(buffer)
                    if kind == "tool":
                        mode = "tool"
                        tool_call = call
                    elif kind == "text":
                        mode = "stream"
                        self.emit_chunk({"content": buffer})
            if finish:
                extra = {key: event[key] for key in ("timings", "samosa", "usage") if key in event}
                if mode == "stream":
                    self.emit_chunk({}, finish=finish, extra=extra)
                elif mode == "undecided":
                    held_finish.append((finish, extra))
        if mode == "undecided":
            kind, call = classify_reply(buffer)
            if kind == "tool" and not final:
                return call, buffer
            if buffer:
                self.emit_chunk({"content": buffer})
            for finish, extra in held_finish:
                self.emit_chunk({}, finish=finish, extra=extra)
            if not held_finish:
                self.emit_chunk({}, finish="stop")
            return None, buffer
        if mode == "tool":
            if final:
                self.emit_chunk({"content": (
                    "I ran out of tool calls before finding a confident answer. "
                    "What I still wanted to run was: " + buffer.strip()
                    + " — ask again to continue, or rephrase."
                )}, finish="stop")
                return None, buffer
            return tool_call, buffer
        return None, buffer

    def shutdown_all(self) -> None:
        supervisor.stopping = True
        supervisor.stop()
        self.server.shutdown()


def main() -> int:
    HOME.mkdir(parents=True, exist_ok=True)
    server = GatewayServer((HOST, PUBLIC_PORT), Handler)
    try:
        supervisor.start()
    except Exception:
        server.server_close()
        raise

    def terminate(_signum: int, _frame: object) -> None:
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGTERM, terminate)
    signal.signal(signal.SIGINT, terminate)
    try:
        server.serve_forever()
    finally:
        supervisor.stop()
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Local DeBERTa decisions for small, allowlisted Jobs actions.

Input and output are JSON. The gateway owns job IDs and persistence; this
helper owns only read-only intent choice and file relevance scoring.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import json
import math
import os
import re
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import time
import traceback

READ_CACHE = None
READER_FINGERPRINT = ""


def cached_pdf_preview(descriptor: int) -> tuple[str, str] | None:
    """Read the host reader's complete matching cache; never accept summaries."""
    if READ_CACHE is None or not READER_FINGERPRINT or os.fstat(descriptor).st_size > MAX_EXTRACT_BYTES:
        return None
    try:
        digest = hashlib.sha256()
        os.lseek(descriptor, 0, os.SEEK_SET)
        while data := os.read(descriptor, 65536):
            digest.update(data)
        key = digest.hexdigest()
        path = Path(READ_CACHE) / key[:2] / (key + '.json')
        with path.open('rb') as cached:
            raw = cached.read((16 << 20) + 1)
        if len(raw) > 16 << 20:
            return None
        entry = json.loads(raw)
        if entry.get('contract_version') != 'reader-v3' or entry.get('pack_fingerprint') != READER_FINGERPRINT:
            return None
        result = json.loads(entry['result'])
        pages = result.get('pages')
        if (result.get('ok') is not True or result.get('retryable') or
                not isinstance(pages, list) or not pages or
                type(result.get('page_count')) is not int or result['page_count'] != len(pages)):
            return None
        for index, page in enumerate(pages, 1):
            if (type(page.get('index')) is not int or page['index'] != index or
                    page.get('needs_review') or page.get('error') or
                    (page.get('inspection') or {}).get('incomplete')):
                return None
            if page.get('source') not in ('text_layer', 'ocr', 'ocr_with_text_layer'):
                return None
            if not isinstance(page.get('lines'), list) or any(not isinstance(line.get('text'), str) for line in page['lines']):
                return None
        for page in pages[:4]:
            text = '\n'.join(line['text'] for line in page['lines'])
            if text.strip():
                return text, 'shared_pdf_cache'
    except (OSError, ValueError, TypeError, KeyError, AttributeError):
        return None
    finally:
        os.lseek(descriptor, 0, os.SEEK_SET)
    return None

os.environ.setdefault("HF_HUB_OFFLINE", "1")
os.environ.setdefault("TRANSFORMERS_OFFLINE", "1")
os.environ.setdefault("USE_TF", "0")

MODEL_NAME = "MoritzLaurer/deberta-v3-large-zeroshot-v2.0"
MODEL_REVISION = "cf44676c28ba7312e5c5f8f8d2c22b3e0c9cdae2"
MODEL_CACHE = (
    Path.home() / ".cache/huggingface/hub"
    / "models--MoritzLaurer--deberta-v3-large-zeroshot-v2.0"
    / "snapshots" / MODEL_REVISION
)
FILE_CRITERIA = {
    "plausible": "The candidate file is a plausible match for the user request based on its filename and preview.",
    "unrelated": "The candidate file appears unrelated to the user request based on its filename and preview.",
}
CHOICES = {
    "find": "Find files that match the user's description.",
    "duplicates": "Find files with identical content.",
    "report": "Count files and summarize what is in the folder.",
    "sort": "Prepare moves that group files by file type.",
    "classify": "Label files by category without moving them.",
    "watch": "Watch this folder for future additions or changes.",
}
NEXT_CHOICES = {
    "refine": "Filter the current shortlist using the user's additional description.",
    "deepen": "Read more content from shortlisted or uncertain files and score them again.",
    "continue": "Score the files that have not yet been checked.",
    "show_all": "Show every score already calculated without further reading.",
}

# This is an operation vocabulary, not a list of recognised questions. The
# same pinned NLI model used by Jobs chooses scope, then one or more operations.
MEMORY_SCOPES = {
    "folder": "Summarize, count, list or identify the whole folder.",
    "files": "Find or read particular files.",
}
MEMORY_OPERATIONS = {
    "overview": "The user wants a summary or overview of what the folder contains.",
    "inventory": "The user wants to list or count files, or find out whether there are files about a subject.",
    "metadata": "The user wants file properties such as PDF page count, size or format.",
    "content": "The user wants to read, summarize, explain or answer questions about file contents.",
}
MEMORY_MEMBERSHIP = {
    "named": "The user names a particular person, organisation, project, filename or identifier.",
    "semantic": "The user describes a general topic, concept or category of documents.",
}


def memory_plan(model, request: dict) -> dict:
    question = request.get("question")
    sources = request.get("sources", [])
    if not isinstance(question, str) or not question.strip() or len(question) > 8192:
        raise DecisionError("A bounded file question is required.")
    if not isinstance(sources, list) or len(sources) > 80:
        raise DecisionError("The memory inventory exceeds the decision budget.")
    previous = request.get("previous_question", "")
    if not isinstance(previous, str) or len(previous) > 4096:
        raise DecisionError("The reference context is invalid.")
    supplied_route = request.get("route")
    if supplied_route is not None:
        if (not isinstance(supplied_route, dict) or supplied_route.get("version") != 1 or
                supplied_route.get("scope") not in MEMORY_SCOPES or
                not isinstance(supplied_route.get("actions"), list) or not supplied_route["actions"] or
                any(action not in MEMORY_OPERATIONS for action in supplied_route["actions"]) or
                len(set(supplied_route["actions"])) != len(supplied_route["actions"])):
            raise DecisionError("The parent memory decision is invalid.")
        scope = {"choice": supplied_route["scope"], "probabilities": supplied_route.get("scope_scores", {})}
        scores, actions = supplied_route.get("action_scores", {}), supplied_route["actions"]
        membership_kind = supplied_route.get("membership_kind", "named")
    else:
        state = "USER: " + question
        if previous:
            state += "\nEarlier user question, only to resolve references in the CURRENT request: " + previous
        scope = model.choice(state=state,
                             instructions="Does the request concern the entire folder or specific files?",
                             criteria=MEMORY_SCOPES)
        if scope.get("choice") not in MEMORY_SCOPES:
            raise DecisionError("The model returned an unknown memory scope.")
        scores = {key: float(model.noul(state=state, instructions=predicate)["noul"])
                  for key, predicate in MEMORY_OPERATIONS.items()}
        if any(not math.isfinite(value) or not 0 <= value <= 1 for value in scores.values()):
            raise DecisionError("The model returned invalid operation probabilities.")
        actions = [key for key, value in scores.items() if value >= 0.5 and
                   (key != "overview" or scope["choice"] == "folder")]
        # A selected memory is contextual evidence for follow-up questions too.
        # An uncertain intent reads bounded evidence; it never fabricates an answer.
        if not actions:
            actions = ["content"]
        membership_kind = "named"
        if scope["choice"] == "files" and "inventory" in actions:
            membership_kind = model.choice(state=state,
                instructions="Is file membership about a named identity or a general topic?",
                criteria=MEMORY_MEMBERSHIP).get("choice")
    if membership_kind not in MEMORY_MEMBERSHIP:
        raise DecisionError("The membership decision is invalid.")
    selections = []
    named = request.get("named_sources", [])
    numbers = {source.get("number") for source in sources if isinstance(source, dict)}
    if (not isinstance(named, list) or any(type(number) is not int or number not in numbers for number in named)
            or len(set(named)) != len(named)):
        raise DecisionError("The resolved file references are invalid.")
    for source in sources:
        if (not isinstance(source, dict) or type(source.get("number")) is not int or
                not isinstance(source.get("path"), str) or
                not isinstance(source.get("preview", ""), str)):
            raise DecisionError("The inventory contains an invalid source.")
        if request.get("verify_proofs"):
            topic = request.get('topic')
            if not isinstance(topic, str) or not 2 <= len(topic) <= 128 or topic.casefold() not in question.casefold():
                raise DecisionError("The quoted topic is invalid.")
            answer = model.choice_fast(
                state=source.get('preview', '')[:1200],
                instructions="Which subject does this document concern?",
                criteria={'plausible': f'This text is about {topic}.',
                          'unrelated': f'This text is not about {topic}.'})
            value = float(answer['probabilities']['plausible'])
            if not math.isfinite(value) or not 0 <= value <= 1:
                raise DecisionError("The proof decision is invalid.")
            choice = "plausible" if value >= .5 else "unrelated"
        elif named:
            choice, value = ("plausible", 1.0) if source["number"] in named else ("unrelated", 0.0)
        elif scope["choice"] == "folder" or "inventory" in actions:
            # Membership/count operations must inspect the whole supplied
            # inventory. Ranking it first would silently turn a shortlist
            # into the universe of files. The host verifies association next.
            choice, value = "plausible", 1.0
        else:
            answer = model.choice_fast(
                state=(f"User request: {question}\n"
                       f"Earlier user reference (not source evidence): {previous}\n"
                       f"File name: {source['path'][:1024]}\n"
                       f"Preview: {source.get('preview', '')[:1200]}"),
                instructions="Is this file worth inspecting next?", criteria=FILE_CRITERIA)
            choice = answer.get("choice")
            value = float(answer["probabilities"]["plausible"])
            if choice not in FILE_CRITERIA or not math.isfinite(value) or not 0 <= value <= 1:
                raise DecisionError("The model returned an invalid source decision.")
        selections.append({"number": source["number"], "choice": choice, "score": value})
    return {"ok": True, "version": 1, "scope": scope["choice"],
            "scope_scores": scope["probabilities"], "actions": actions,
            "action_scores": scores, "sources": selections,
            "membership_kind": membership_kind,
            "model": MODEL_NAME, "revision": MODEL_REVISION}
SHORT_CHARS = 1200
DEEP_CHARS = 3200
BATCH_FILES = 250
MAX_EXTRACT_BYTES = 64 * 1024 * 1024
MAX_INVENTORY_FILES = 10000
MAX_INVENTORY_DIRECTORIES = 20000
MAX_FOLDER_DECISIONS = 200
MAX_INVENTORY_SECONDS = 60
MAX_INVENTORY_DEPTH = 32
MAX_INVENTORY_FILE_BYTES = 64 * 1024 * 1024
MAX_ELIGIBLE_BYTES = 2 * 1024 * 1024 * 1024
GENERATED_DIRECTORIES = {
    ".git", ".svn", ".hg", "node_modules", ".cache", "__pycache__",
    ".venv", "venv", "env", "target", "build", "dist", "deriveddata",
    ".trash", ".mypy_cache", ".pytest_cache", ".ruff_cache",
    "site-packages", ".next", ".nuxt", ".yarn", ".pnpm-store",
    ".gradle", "coverage", ".idea",
}
PACKAGE_SUFFIXES = (".app", ".bundle", ".framework", ".plugin", ".xcodeproj",
                    ".xcworkspace", ".photoslibrary")


class DecisionError(Exception):
    pass


PROGRESS = None


def progress(event: dict) -> None:
    if PROGRESS is not None:
        try:
            PROGRESS.write(json.dumps(event, ensure_ascii=False, separators=(",", ":")) + "\n")
            PROGRESS.flush()
        except OSError as error:
            print(f"Jobs progress could not be saved: {error}", file=sys.stderr)


def engine():
    import torch
    from opendecision import OpenDecisionEngine

    model = MODEL_CACHE
    if not (model / "model.safetensors").exists():
        raise DecisionError("The pinned DeBERTa decision model is not installed locally.")
    torch.set_num_threads(4)
    with contextlib.redirect_stdout(sys.stderr):
        return OpenDecisionEngine(model=str(model), device=-1, batch_size=8)


def route(model, goal: str) -> dict:
    answer = model.choice(
        state=f"USER: {goal}",
        instructions="Which file action does the user want?",
        criteria=CHOICES,
    )
    return {
        "ok": True,
        "action": answer["choice"],
        "scores": answer["probabilities"],
        "model": MODEL_NAME,
        "revision": MODEL_REVISION,
    }


def inventory(folder: str, model, goal: str,
              prior_decisions: list[dict] | None = None,
              scan_folders: bool = True) -> tuple[list[dict], dict]:
    """Enumerate candidate documents under the bounded source policy.

    Folder names do not establish relevance or authorize dropping their files.
    This scanner opens eligible directories and records file metadata.
    Content is opened later by open_inventory_file with dev/ino checks.
    """
    rows: list[dict] = []
    decisions: list[dict] = []
    deadline = time.monotonic() + MAX_INVENTORY_SECONDS
    counters = {"directories": 0, "eligible_bytes": 0, "skipped_folders": 0,
                "model_folders": 0, "folders_pending": False,
                "partial": False, "limiting_reason": "none"}
    root_fd = os.open(folder, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    root_dev = os.fstat(root_fd).st_dev

    def stop(reason: str) -> None:
        counters["partial"] = True
        counters["limiting_reason"] = reason

    def walk(directory_fd: int, relative: str, depth: int) -> None:
        if counters["partial"]:
            return
        if time.monotonic() >= deadline:
            stop("deadline")
            return
        counters["directories"] += 1
        try:
            with os.scandir(directory_fd) as entries:
                names = [entry.name for entry in entries]
            def file_first(name: str) -> tuple[int, str]:
                try:
                    info = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
                    return (1 if stat.S_ISDIR(info.st_mode) else 0, name.casefold())
                except OSError:
                    return (2, name.casefold())
            names.sort(key=file_first)
        except OSError:
            counters["skipped_folders"] += 1
            return
        for name in names:
            if counters["partial"]:
                break
            if time.monotonic() >= deadline:
                stop("deadline")
                break
            path = f"{relative}/{name}" if relative else name
            if name.startswith(".") or name.casefold() in GENERATED_DIRECTORIES:
                if not scan_folders:
                    continue
                try:
                    excluded_info = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
                except OSError:
                    continue
                if stat.S_ISDIR(excluded_info.st_mode):
                    counters["skipped_folders"] += 1
                    decision = {"path": path, "score": None, "search": False,
                                "reason": "generated_tree"}
                    if len(decisions) < MAX_FOLDER_DECISIONS:
                        decisions.append(decision)
                        progress({"type": "folder_decision", **decision})
                continue
            try:
                info = os.stat(name, dir_fd=directory_fd, follow_symlinks=False)
            except OSError:
                continue
            if info.st_dev != root_dev or stat.S_ISLNK(info.st_mode):
                continue
            if stat.S_ISDIR(info.st_mode):
                if not scan_folders:
                    counters["folders_pending"] = True
                    continue
                if depth >= MAX_INVENTORY_DEPTH:
                    counters["skipped_folders"] += 1
                    continue
                if counters["directories"] >= MAX_INVENTORY_DIRECTORIES:
                    stop("maximum_directories")
                    break
                progress({"type": "folder_checking", "path": path})
                package = name.casefold().endswith(PACKAGE_SUFFIXES)
                selected = not package
                decision = {"path": path, "score": None, "search": selected,
                            "reason": "application_bundle" if package else "inventory_policy"}
                if len(decisions) < MAX_FOLDER_DECISIONS:
                    decisions.append(decision)
                progress({"type": "folder_decision", **decision, "cached": False})
                if not selected:
                    counters["skipped_folders"] += 1
                    continue
                try:
                    child_fd = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                                       dir_fd=directory_fd)
                    opened = os.fstat(child_fd)
                    if opened.st_dev != info.st_dev or opened.st_ino != info.st_ino:
                        os.close(child_fd)
                        counters["skipped_folders"] += 1
                        continue
                except OSError:
                    counters["skipped_folders"] += 1
                    continue
                try:
                    walk(child_fd, path, depth + 1)
                finally:
                    os.close(child_fd)
                continue
            if not stat.S_ISREG(info.st_mode) or info.st_size > MAX_INVENTORY_FILE_BYTES:
                continue
            if len(rows) >= MAX_INVENTORY_FILES:
                stop("maximum_files")
                break
            if info.st_size > MAX_ELIGIBLE_BYTES - counters["eligible_bytes"]:
                stop("maximum_eligible_bytes")
                break
            rows.append({"rel_path": path, "size": info.st_size,
                         "dev": info.st_dev, "ino": info.st_ino,
                         "mtime_ns": str(info.st_mtime_ns)})
            counters["eligible_bytes"] += info.st_size

    try:
        walk(root_fd, "", 0)
    finally:
        os.close(root_fd)
    return rows, {**counters, "folder_decisions": decisions}


def open_inventory_file(root_fd: int, row: dict) -> int:
    relative = row.get("rel_path")
    if not isinstance(relative, str) or not relative or relative.startswith("/"):
        raise DecisionError("The inventory returned an invalid relative path.")
    parts = relative.split("/")
    if any(part in ("", ".", "..") for part in parts):
        raise DecisionError("The inventory returned an invalid relative path.")
    directory_fd = os.dup(root_fd)
    try:
        for part in parts[:-1]:
            next_fd = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                              dir_fd=directory_fd)
            os.close(directory_fd)
            directory_fd = next_fd
        file_fd = os.open(parts[-1], os.O_RDONLY | os.O_NOFOLLOW,
                          dir_fd=directory_fd)
    finally:
        os.close(directory_fd)
    info = os.fstat(file_fd)
    if not stat.S_ISREG(info.st_mode) or (
        info.st_dev != int(row["dev"]) or info.st_ino != int(row["ino"])
        or ("size" in row and info.st_size != int(row["size"]))
        or ("mtime_ns" in row and info.st_mtime_ns != int(row["mtime_ns"]))
    ):
        os.close(file_fd)
        raise DecisionError("A file changed after inventory; it was not read.")
    return file_fd


def ocr_image(path: Path, ocr: str) -> str:
    if not ocr:
        return ""
    try:
        run = subprocess.run([ocr, "recognize", str(path)], capture_output=True,
                             text=True, timeout=18, check=False)
        payload = json.loads(run.stdout) if run.returncode == 0 else {}
    except (OSError, subprocess.TimeoutExpired, json.JSONDecodeError):
        return ""
    return str(payload.get("text") or "") if payload.get("ok") else ""


def pdf_text(file_fd: int, extract: str, ocr: str) -> tuple[str, str]:
    with tempfile.TemporaryDirectory(prefix="samosa-decision-pdf-") as temporary:
        copied = Path(temporary) / "document.pdf"
        with copied.open("wb") as output:
            remaining = MAX_EXTRACT_BYTES
            while remaining:
                chunk = os.read(file_fd, min(65536, remaining))
                if not chunk:
                    break
                output.write(chunk)
                remaining -= len(chunk)
        if os.read(file_fd, 1):
            return "", "name_only"
        try:
            run = subprocess.run([extract, "--json-pages", str(copied), "1", "1"],
                                 capture_output=True, text=True, timeout=12, check=False)
            payload = json.loads(run.stdout) if run.returncode == 0 else {}
        except (OSError, subprocess.TimeoutExpired, json.JSONDecodeError):
            payload = {}
        text = str(payload.get("text") or "") if payload.get("ok") else ""
        if text.strip():
            return text, "pdf_text"
        image = Path(temporary) / "page.ppm"
        try:
            rendered = subprocess.run([extract, "--render-ocr-ppm", str(copied),
                                       "1", str(image)], capture_output=True,
                                      timeout=12, check=False)
        except (OSError, subprocess.TimeoutExpired):
            rendered = None
        text = ocr_image(image, ocr) if rendered and not rendered.returncode and image.exists() else ""
        if text.strip():
            return text, "ocr"
        page_count = int(payload.get("page_count") or 0) if payload.get("ok") else 0
        if page_count > 1:
            try:
                later = subprocess.run([extract, "--json-pages", str(copied), "2",
                                        str(min(3, page_count - 1))],
                                       capture_output=True, text=True, timeout=12, check=False)
                later_payload = json.loads(later.stdout) if later.returncode == 0 else {}
                later_text = str(later_payload.get("text") or "") if later_payload.get("ok") else ""
                if later_text.strip():
                    return later_text, "pdf_text"
            except (OSError, subprocess.TimeoutExpired, json.JSONDecodeError):
                pass
        return "", "name_only"


def image_text(file_fd: int, ocr: str) -> tuple[str, str]:
    with tempfile.TemporaryDirectory(prefix="samosa-decision-image-") as temporary:
        copied = Path(temporary) / "image"
        with copied.open("wb") as output:
            remaining = MAX_EXTRACT_BYTES
            while remaining:
                chunk = os.read(file_fd, min(65536, remaining))
                if not chunk:
                    break
                output.write(chunk)
                remaining -= len(chunk)
        if os.read(file_fd, 1):
            return "", "name_only"
        text = ocr_image(copied, ocr)
        return (text, "ocr") if text.strip() else ("", "name_only")


def document_text(file_fd: int, extract: str, suffix: str) -> tuple[str, str]:
    if not extract:
        return "", "name_only"
    with tempfile.TemporaryDirectory(prefix="samosa-decision-document-") as temporary:
        copied = Path(temporary) / ("document" + suffix)
        with copied.open("wb") as output:
            remaining = MAX_EXTRACT_BYTES
            while remaining:
                chunk = os.read(file_fd, min(65536, remaining))
                if not chunk:
                    break
                output.write(chunk)
                remaining -= len(chunk)
        if os.read(file_fd, 1):
            return "", "name_only"
        try:
            run = subprocess.run([extract, "--json", str(copied)],
                                 capture_output=True, text=True, timeout=12, check=False)
            payload = json.loads(run.stdout) if run.returncode == 0 else {}
        except (OSError, subprocess.TimeoutExpired, json.JSONDecodeError):
            return "", "name_only"
        content = str(payload.get("text") or "") if payload.get("ok") else ""
        return (content, "document_text") if content.strip() else ("", "name_only")


def excerpt(root_fd: int, row: dict, extract: str, ocr: str,
            characters: int) -> tuple[str, str]:
    try:
        descriptor = open_inventory_file(root_fd, row)
    except DecisionError:
        return "", "changed_source"
    except OSError:
        return "", "unreadable"
    try:
        before = os.fstat(descriptor)
        name = row["rel_path"].lower()
        if name.endswith(".pdf"):
            cached = cached_pdf_preview(descriptor)
            content, source = cached if cached is not None else pdf_text(descriptor, extract, ocr)
        elif name.endswith((".png", ".jpg", ".jpeg", ".tif", ".tiff", ".bmp")):
            content, source = image_text(descriptor, ocr)
        elif name.endswith((".docx", ".html", ".htm")):
            content, source = document_text(descriptor, extract, Path(name).suffix)
        else:
            data = os.read(descriptor, 8192 if characters == SHORT_CHARS else 32768)
            if data.startswith((b"\xff\xfe", b"\xfe\xff")):
                try:
                    content = data.decode("utf-16")
                except UnicodeDecodeError:
                    content = ""
            elif b"\0" in data:
                content = ""
            else:
                try:
                    content = data.decode("utf-8-sig")
                except UnicodeDecodeError:
                    content = ""
            visible = "".join(content.split())
            if (any(ord(char) < 32 and char not in "\t\r\n" for char in content)
                    or (len(visible) >= 16 and sum(char.isalpha() for char in visible) < 3)):
                content = ""
            source = "text" if content.strip() else "name_only"
        after = os.fstat(descriptor)
        if (before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (after.st_size, after.st_mtime_ns, after.st_ctime_ns):
            return "", "changed_source"
        return " ".join(content.split())[:characters], source
    finally:
        os.close(descriptor)


def search_intent(request: dict) -> dict:
    intent = request.get("search_intent")
    if (not isinstance(intent, dict) or intent.get("kind") not in ("document_type", "topic", "name")
            or not isinstance(intent.get("subject"), str) or not 2 <= len(intent["subject"]) <= 256
            or not isinstance(intent.get("target"), str) or not 2 <= len(intent["target"]) <= 768):
        raise DecisionError("A validated search interpretation is required. Run the request again.")
    constraints = intent.get("constraints")
    if (not isinstance(constraints, list) or len(constraints) > 4 or
            any(not isinstance(text, str) or not 2 <= len(text) <= 160 for text in constraints)):
        raise DecisionError("Validated search conditions are required. Run the request again.")
    result = {key: intent[key] for key in ("subject", "kind", "target", "constraints")}
    if "requested_role" in intent:
        if intent["requested_role"] not in (*DOCUMENT_ROLES, "any"):
            raise DecisionError("Invalid requested document role.")
        result["requested_role"] = intent["requested_role"]
    if "document_role" in intent:
        result["document_role"] = checked_choice(intent["document_role"], DOCUMENT_ROLES)
    return result


# A general document-role hierarchy separates forms from instructions/notices.
# Topic and identity searches intentionally span roles; type searches do not.
DOCUMENT_IDENTIFIER = re.compile(r"(?<!\w)[A-Za-z]{1,5}[- ]?\d{1,6}[A-Za-z0-9]*(?!\w)")
FORM_IDENTIFIER = re.compile(r"\bform\s+([A-Za-z]{1,5}[- ]?\d{1,6}[A-Za-z0-9]*)(?!\w)", re.I)


def identifier_decision(intent: dict, text: str) -> str | None:
    expected = {re.sub(r"[^a-z0-9]", "", match.group().lower()) for match in DOCUMENT_IDENTIFIER.finditer(intent["subject"])}
    if not expected:
        return None
    if (intent.get("document_role") or {}).get("choice") != "form":
        observed = {re.sub(r"[^a-z0-9]", "", match.group().lower()) for match in DOCUMENT_IDENTIFIER.finditer(text)}
        return "supported" if observed & expected else "identifier_not_readable"
    actual = FORM_IDENTIFIER.search(text)
    if actual:
        identity = re.sub(r"[^a-z0-9]", "", actual.group(1).lower())
        return "supported" if identity in expected else "different_identifier"
    return "identifier_not_readable"


DOCUMENT_ROLES = {
    "form": "This is an application form or a blank or completed form with fields to fill in.",
    "instructions": "This is an instruction guide explaining how to fill in or submit a form.",
    "notice": "This is a notice, confirmation or receipt reporting the status of an application.",
    "other": "This is a narrative document such as a report, research paper, essay, letter or meeting notes.",
}


def checked_choice(answer: dict, criteria: dict) -> dict:
    try:
        scores = {key: float(answer["probabilities"][key]) for key in criteria}
        valid = (answer["choice"] in criteria and all(math.isfinite(value) and 0 <= value <= 1 for value in scores.values()))
    except (KeyError, TypeError, ValueError):
        valid = False
    if not valid:
        raise DecisionError("The decision model returned invalid file scores.")
    return {"choice": answer["choice"], "probabilities": scores}


def interpret_document_role(model, intent: dict) -> dict:
    if intent["kind"] == "document_type" and "document_role" not in intent:
        intent["document_role"] = checked_choice(model.choice_fast(state=intent["target"],
            instructions="What kind of document is requested?", criteria=DOCUMENT_ROLES), DOCUMENT_ROLES)
    if intent["kind"] == "document_type" and intent.get("requested_role") in DOCUMENT_ROLES:
        # The validated query interpretation chooses the requested scope; the
        # NLI role probabilities remain diagnostics, never invented confidence.
        intent["document_role"]["choice"] = intent["requested_role"]
    return intent


def covered_type_variants(model, intent: dict) -> dict[str, dict]:
    """Recognize an inclusive union already covered by the chosen type.

    Both literal grounding in the fixed role definition and independent NLI
    entailment are required. This cannot erase a new name, date, signature or
    approval requirement, and does not recognize any form number or question.
    """
    expected = intent.get("document_role")
    if intent["kind"] != "document_type" or not expected:
        return {}
    definition = DOCUMENT_ROLES[expected["choice"]]
    words = set(re.findall(r"\w+", definition.casefold()))
    covered = {}
    for requirement in intent["constraints"]:
        terms = set(re.findall(r"\w+", requirement.casefold()))
        if "or" not in terms or not terms <= words:
            continue
        answer = checked_choice(model.choice_fast(state=definition,
            instructions="Does this definition already cover the requested alternatives?",
            criteria={"covered": f"This document is {requirement}.",
                      "additional": f"This requirement is not established: {requirement}."}),
            {"covered": "", "additional": ""})
        if answer["choice"] == "covered" and answer["probabilities"]["covered"] >= .9:
            covered[requirement] = answer
    return covered


def preview_windows(model, text: str) -> list[str]:
    classifier = vars(model).get("classifier")
    tokenizer = getattr(classifier, "tokenizer", None) if classifier is not None else None
    windows, offset = [], 0
    while offset < max(1, len(text)):
        width = min(1000, max(1, len(text) - offset))
        if tokenizer is not None:
            while width > 1 and len(tokenizer.encode(text[offset:offset + width], add_special_tokens=False)) > 256:
                width = max(1, width // 2)
        windows.append(text[offset:offset + width])
        if offset + width >= len(text):
            break
        offset += width - min(160, width // 5)
    return windows


def score(model, intent: dict, items: list[dict]) -> None:
    intent = interpret_document_role(model, search_intent({"search_intent": intent}))
    target = intent["target"].strip().rstrip(".")
    variants = covered_type_variants(model, intent)
    criteria = {"plausible": f"This document is {target}.",
                "unrelated": f"This document is not {target}."}
    for item in items:
        progress({"type": "file_checking", "path": item["path"]})
        # Bound source tokens independently from the dynamic hypotheses.
        # Inspect overlapping windows rather than silently losing later text.
        text = item["excerpt"]
        windows = preview_windows(model, text)
        answers = []
        for window in windows:
            state = window if intent["kind"] == "document_type" and window.strip() else f"File name: {item['path'][:128]}\nPreview: {window}"
            answer = model.choice_fast(state=state, instructions="Classify the document itself.", criteria=criteria)
            answer = checked_choice(answer, criteria)
            value = answer["probabilities"]["plausible"]
            role = None
            identity = None
            if intent["kind"] == "document_type" and window.strip():
                # The primary identifier belongs to the document, not each
                # window. Later references cannot replace a different header ID.
                identity = identifier_decision(intent, text)
            if intent["kind"] == "document_type" and (value > 0.2 or identity == "supported") and window.strip():
                expected = intent["document_role"]
                role = checked_choice(model.choice_fast(state=window,
                    instructions="What kind of document is this?", criteria=DOCUMENT_ROLES), DOCUMENT_ROLES)
                explicit_role = intent.get("requested_role") == expected["choice"]
                expected_score = expected["probabilities"][expected["choice"]]
                actual_score = role["probabilities"][role["choice"]]
                if (not explicit_role and expected_score < 0.5) or actual_score < 0.6:
                    value = min(value, 0.5)  # unresolved role stays in review
                elif role["choice"] != expected["choice"]:
                    value = min(value, 0.19)  # a mention is not the requested type
                if identity == "different_identifier":
                    value = min(value, 0.19)
                elif identity == "identifier_not_readable":
                    value = min(value, 0.5)
            supported_type = (identity == "supported" and role is not None and
                role["choice"] == intent["document_role"]["choice"] and
                role["probabilities"][role["choice"]] >= .6 and
                (intent.get("requested_role") == intent["document_role"]["choice"] or
                 intent["document_role"]["probabilities"][intent["document_role"]["choice"]] >= .5))
            conditions = []
            for constraint in intent["constraints"]:
                if constraint in variants:
                    continue
                condition = checked_choice(model.choice_fast(state=state,
                    instructions="Does the source satisfy this additional requirement?",
                    criteria={"plausible": f"This document satisfies the requirement: {constraint}.",
                              "unrelated": f"This document does not satisfy the requirement: {constraint}."}), criteria)
                conditions.append({"requirement": constraint, **condition})
            if conditions:
                condition_score = min(check["probabilities"]["plausible"] for check in conditions)
                if supported_type:
                    value = condition_score
                supported_type = supported_type and condition_score >= .8
                if condition_score < .8:
                    value = min(value, condition_score)
            answers.append((value, state, answer, role, identity, supported_type, conditions))
        value, state, answer, role, identity, typed_support, conditions = max(answers, key=lambda row: (row[5], row[0]))
        item["search_intent"] = intent
        item["type_variant_evidence"] = variants
        item["condition_evidence"] = conditions
        item["identifier_evidence"] = identity
        item["document_role"] = role
        item["model_state"] = state
        item["model_instructions"] = "Classify the document itself."
        item["model_criteria"] = criteria
        item["model_choice"] = answer["choice"]
        item["model_scores"] = answer["probabilities"]
        missing = item["source"] in ("name_only", "unreadable", "changed_source") or not text.strip()
        # Exact identifier evidence plus agreement on document role provides
        # independent support. NLI probabilities alone are not calibrated
        # probabilities of correctness; a strong literal ID must still be the
        # requested type, and a merely similar ID cannot pass this gate.
        item["needs_check"] = missing or (0.2 < value < 0.8 and not typed_support)
        item["decision"] = "needs_check" if item["needs_check"] else "match" if value >= 0.8 or typed_support else "rejected"
        item["score"] = value
        item["reason"] = "no_readable_excerpt" if missing else "ambiguous_preview" if item["needs_check"] else "different_document_identifier" if identity == "different_identifier" else "identifier_and_role" if typed_support else "model_choice"
        progress({"type": "file_decision", "path": item["path"],
                  "score": item["score"], "model_score": value,
                  "model_choice": answer["choice"],
                  "choice_score": answer["probabilities"][answer["choice"]],
                  "decision": item["decision"],
                  "reason": item["reason"], "source": item["source"],
                  "excerpt_chars_actual": len(item["excerpt"])})


def sorted_items(items: list[dict]) -> list[dict]:
    return sorted(items, key=lambda item: (-item.get("score", 0), item["path"]))


def result(goal: str, folder: str, items: list[dict], count: int,
           next_offset: int, done: dict, steps: list[dict]) -> dict:
    ranked = sorted_items(items)
    shortlist_count = sum(item.get("decision") == "match" for item in ranked)
    return {
        "ok": True, "schema_version": 7, "goal": goal, "folder": folder,
        "items": ranked, "shortlist_count": shortlist_count,
        "total_files": count, "checked_files": len(items),
        "next_offset": next_offset, "remaining_files": max(0, count - next_offset),
        "partial": bool(done["partial"]), "limiting_reason": done["limiting_reason"],
        "folders_pending": bool(done["folders_pending"]),
        "folder_decisions": done["folder_decisions"],
        "skipped_folders": done["skipped_folders"],
        "model_folders": done["model_folders"],
        "needs_check_count": sum(bool(item.get("needs_check")) for item in ranked),
        "steps": steps, "model": MODEL_NAME,
        "revision": MODEL_REVISION,
    }


def start(model, request: dict, fs: str, extract: str, ocr: str,
          previous: dict | None) -> dict:
    folder = request.get("folder") or (previous or {}).get("folder")
    goal = request.get("goal") or (previous or {}).get("goal")
    if not isinstance(folder, str) or not folder or not isinstance(goal, str) or not goal:
        raise DecisionError("A folder and a natural-language request are required.")
    intent = interpret_document_role(model, search_intent(request))
    progress({"type": "search_intent", **intent})
    root = os.path.realpath(folder)
    if not os.path.isdir(root):
        raise DecisionError("The selected folder cannot be opened by Samosa.")
    prior_items = list((previous or {}).get("items", []))
    processed_paths = {item["path"] for item in prior_items}
    progress({"type": "decision_stage", "message": "Checking files directly in the selected folder first"})
    root_rows, root_done = inventory(root, model, goal, scan_folders=False)
    root_batch = [row for row in root_rows if row["rel_path"] not in processed_paths]
    rows, done = root_rows, root_done
    root_fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    added = []
    def check_rows(batch: list[dict]) -> None:
        for row in batch:
            progress({"type": "file_pending", "path": row["rel_path"]})
        for row in batch:
            progress({"type": "file_checking", "path": row["rel_path"]})
            text, source = excerpt(root_fd, row, extract, ocr, SHORT_CHARS)
            item = {"path": row["rel_path"], "excerpt": text, "source": source,
                    "excerpt_chars": SHORT_CHARS,
                    "dev": row["dev"], "ino": row["ino"], "size": row["size"],
                    **({"mtime_ns": row["mtime_ns"]} if "mtime_ns" in row else {})}
            score(model, intent, [item])
            if item["needs_check"] and source not in ("name_only", "unreadable", "changed_source") and len(text) == SHORT_CHARS:
                progress({"type": "file_reading", "path": item["path"], "message": "Reading more evidence"})
                item["excerpt"], item["source"] = excerpt(root_fd, row, extract, ocr, DEEP_CHARS)
                item["excerpt_chars"] = DEEP_CHARS
                score(model, intent, [item])
            added.append(item)
            processed_paths.add(row["rel_path"])
    try:
        check_rows(root_batch)
        if root_done["folders_pending"] and not root_done["partial"]:
            progress({"type": "decision_stage", "message": "Direct files checked; checking eligible subfolders"})
            rows, done = inventory(root, model, goal, (previous or {}).get("folder_decisions"))
            progress({"type": "decision_stage", "message": f"Found {len(rows)} candidate files in selected folders"})
            remaining_batch = [row for row in rows if row["rel_path"] not in processed_paths]
            check_rows(remaining_batch[:BATCH_FILES])
    finally:
        os.close(root_fd)
    steps = [
        {"action": "decide_folders", "folders": len(done["folder_decisions"]),
         "skipped": done["skipped_folders"]},
        {"action": "inventory", "files": len(rows), "partial": bool(done.get("partial"))},
        {"action": "read_previews", "characters_per_file": SHORT_CHARS, "files": len(added)},
        {"action": "score_matches", "files": len(added)},
        {"action": "show_shortlist", "files": len(prior_items) + len(added)},
    ]
    response = result(goal, root, prior_items + added, len(rows), len(processed_paths), done, steps)
    response["search_intent"] = intent
    return response


def next_action_choice(model, request: dict, previous: dict) -> dict:
    if previous.get("schema_version") != 7:
        raise DecisionError("This shortlist used the old scoring method. Run the request again.")
    followup = request.get("followup", "")
    if not isinstance(followup, str):
        raise DecisionError("The follow-up request must be text.")
    if not followup.strip():
        followup = "Check the likely and uncertain files more thoroughly."
    selected_paths = request.get("selected_paths", [])
    available_paths = {item["path"] for item in previous["items"]}
    if (not isinstance(selected_paths, list) or len(selected_paths) > 50 or
        any(not isinstance(path, str) or path not in available_paths for path in selected_paths)):
        raise DecisionError("Select up to 50 files from this job.")
    selected_paths = list(dict.fromkeys(selected_paths))
    choices = dict(NEXT_CHOICES)
    if selected_paths:
        choices = {key: choices[key] for key in ("refine", "deepen")}
    elif not previous.get("remaining_files") and not previous.get("folders_pending"):
        choices.pop("continue")
    action_override = request.get("action")
    if action_override is not None and action_override not in choices:
        raise DecisionError("That action is not available for this selection.")
    if action_override:
        selected = action_override
        progress({"type": "action_decision", "action": selected, "score": None,
                  "chosen_by": "user"})
    else:
        action_answer = model.choice(
            state=f"USER: {followup}\nCURRENT JOB: {previous['goal']}\n"
                  f"CHECKED: {previous['checked_files']} files",
            instructions="Which single small action should Jobs run next?",
            criteria=choices,
        )
        selected = action_answer["choice"]
        progress({"type": "action_decision", "action": selected,
                  "score": action_answer["probabilities"][selected],
                  "chosen_by": "model"})
    return {"ok": True, "action": selected, "followup": followup, "selected_paths": selected_paths}


def next_action(model, request: dict, previous: dict, fs: str, extract: str,
                ocr: str) -> dict:
    choice = next_action_choice(model, request, previous)
    selected, followup, selected_paths = choice["action"], choice["followup"], choice["selected_paths"]
    if selected == "continue":
        response = start(model, previous, fs, extract, ocr, previous)
    elif selected == "show_all":
        response = dict(previous)
        response["steps"] = [{"action": "show_all_scores", "files": len(response["items"])}]
    else:
        response = dict(previous)
        ranked = sorted_items(list(previous["items"]))
        target_paths = set(selected_paths) if selected_paths else {item["path"] for item in ranked if
            selected == "refine" or item.get("decision") != "rejected"}
        intent = interpret_document_role(model, search_intent(request if selected == "refine" else previous))
        if selected == "deepen":
            if not selected_paths:
                target_paths.update(item["path"] for item in ranked if item["source"] == "name_only")
            root_fd = os.open(previous["folder"], os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
            try:
                for item in response["items"]:
                    if item["path"] in target_paths:
                        row = {"rel_path": item["path"], "dev": item["dev"],
                               "ino": item["ino"], "size": item["size"],
                               **({"mtime_ns": item["mtime_ns"]} if "mtime_ns" in item else {})}
                        progress({"type": "file_checking", "path": item["path"]})
                        text, source = excerpt(root_fd, row, extract, ocr, DEEP_CHARS)
                        if text:
                            item["excerpt"], item["source"] = text, source
                            item["excerpt_chars"] = DEEP_CHARS
                        else:
                            item["excerpt"], item["source"] = "", source
                            item["excerpt_chars"] = DEEP_CHARS
            finally:
                os.close(root_fd)
            selected_items = [item for item in response["items"] if item["path"] in target_paths]
            groups = {}
            for item in selected_items:
                item_intent = search_intent({"search_intent": item.get("search_intent", intent)})
                key = json.dumps(item_intent, sort_keys=True)
                groups.setdefault(key, (item_intent, []))[1].append(item)
            for item_intent, group in groups.values():
                score(model, item_intent, group)
            response["steps"] = [{"action": "read_previews", "characters_per_file": DEEP_CHARS, "files": len(selected_items)},
                                  {"action": "score_matches", "files": len(selected_items)}]
        else:
            selected_items = [item for item in response["items"] if item["path"] in target_paths]
            score(model, intent, selected_items)
            if not selected_paths:
                response["search_intent"] = intent
                response["goal"] = previous["goal"] + "\nAdditional constraints: " + followup
            response["steps"] = [{"action": "refine_shortlist", "files": len(selected_items)}]
        response["items"] = sorted_items(response["items"])
        response["needs_check_count"] = sum(bool(item.get("needs_check")) for item in response["items"])
        response["shortlist_count"] = sum(item.get("decision") == "match" for item in response["items"])
    response["selected_action"] = selected
    response["focus_paths"] = selected_paths
    response["asked"] = followup
    return response


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("route", "start", "next", "next_route", "memory"), required=True)
    parser.add_argument("--request", required=True)
    parser.add_argument("--previous")
    parser.add_argument("--fs")
    parser.add_argument("--extract")
    parser.add_argument("--ocr")
    parser.add_argument("--progress")
    parser.add_argument("--read-cache")
    parser.add_argument("--reader-fingerprint", default="")
    args = parser.parse_args()
    global PROGRESS, READ_CACHE, READER_FINGERPRINT
    READ_CACHE, READER_FINGERPRINT = args.read_cache, args.reader_fingerprint
    try:
        if args.progress:
            descriptor = os.open(args.progress, os.O_WRONLY | os.O_CREAT | os.O_APPEND | os.O_NOFOLLOW, 0o600)
            PROGRESS = os.fdopen(descriptor, "w", encoding="utf-8", buffering=1)
            progress({"type": "decision_stage", "message": "Loading DeBERTa"})
        request = json.loads(Path(args.request).read_text(encoding="utf-8"))
        previous = json.loads(Path(args.previous).read_text(encoding="utf-8")) if args.previous else None
        model = engine()
        if args.mode == "route":
            goal = request.get("goal")
            if not isinstance(goal, str) or not goal.strip():
                raise DecisionError("Describe what you want Jobs to do.")
            response = route(model, goal)
        elif args.mode == "memory":
            response = memory_plan(model, request)
        elif args.mode == "start":
            response = start(model, request, args.fs, args.extract, args.ocr, None)
        else:
            if not previous:
                raise DecisionError("The saved shortlist is unavailable.")
            response = next_action_choice(model, request, previous) if args.mode == "next_route" else next_action(model, request, previous, args.fs, args.extract, args.ocr)
    except Exception as error:
        traceback.print_exc(file=sys.stderr)
        message = str(error) or type(error).__name__
        response = {"ok": False, "error": message}
        progress({"type": "decision_error", "message": message})
    finally:
        if PROGRESS is not None:
            PROGRESS.close()
            PROGRESS = None
    print(json.dumps(response, ensure_ascii=False, separators=(",", ":")))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Generate the docs/api reference from lemond's route specs.

Every HTTP route declares its documentation in its RouteSpec (see
docs/dev/specs/http-server.md). This script follows gen_mcp_boilerplate.py: it
starts a lemond through the Lemond harness, reads every RouteSpec from
GET /internal/routes, and rewrites only the marker-delimited regions of the
docs/api pages:

    <!-- BEGIN GENERATED: <page>.summary -->   the page's summary table; rows that
                                               link to no route are hand-written
    <!-- BEGIN GENERATED: <route id> -->      one route's reference section

Each route section shows real requests and the responses lemond returned to
them. Those responses are recorded in docs/tools/api_examples_cache.json, keyed
by everything that determines them, and an example runs only when its entry is
missing. --check runs no examples and loads no model: it fails when any region,
cache entry or recorded response is stale or invalid.

Usage:
    python docs/tools/gen_api_boilerplate.py [--lemond PATH] [--check] [--bin-dir DIR]
"""

from __future__ import annotations

import argparse
import base64
import collections
import copy
import difflib
import hashlib
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from pathlib import Path

from gen_backend_boilerplate import Lemond, find_lemond

REPO_ROOT = Path(__file__).resolve().parents[2]
API_DOCS = REPO_ROOT / "docs" / "api"
CACHE_FILE = REPO_ROOT / "docs" / "tools" / "api_examples_cache.json"
FIXTURES = REPO_ROOT / "docs" / "tools" / "fixtures"
BACKEND_VERSIONS = REPO_ROOT / "src" / "cpp" / "resources" / "backend_versions.json"
SERVER_MODELS = REPO_ROOT / "src" / "cpp" / "resources" / "server_models.json"

DOCS_HOST = "http://localhost:13305"
JSON_FORMATS = {"Json", "JsonLines", "EventStream"}
STREAM_HEAD = 3
STREAM_TAIL = 2
TEXT_LINES = 20
# Responses can carry whole images as base64. Strings this long are shortened when
# recorded, so the docs show the shape of the field instead of megabytes of data.
LONG_STRING = 1000
LONG_STRING_KEPT = 80
LONG_ARRAY = 16
EXAMPLE_TIMEOUT = 1800

PAGE_TITLES = {
    "openai": "OpenAI-Compatible API",
    "lemonade": "Lemonade API",
    "llamacpp": "llama.cpp-Specific API",
    "router": "Router API",
    "internal": "Internal API",
    "ollama": "Ollama-Compatible API",
    "anthropic": "Anthropic-Compatible API",
    "mcp": "MCP Gateway",
}

QUAD_PREFIXES = ["/api/v0/", "/api/v1/", "/v0/", "/v1/"]
BADGE = "<sub>![Status](https://img.shields.io/badge/{})</sub>"
BINARY_EXTENSIONS = {
    "audio/mpeg": "mp3",
    "audio/wav": "wav",
    "audio/opus": "opus",
    "audio/aac": "aac",
    "audio/flac": "flac",
    "model/gltf-binary": "glb",
}


class GenerationError(RuntimeError):
    pass


# --------------------------------------------------------------------------
# Route specs
# --------------------------------------------------------------------------


def page_of(route_id: str) -> str:
    return route_id.split(".", 1)[0]


def urls_for(spec: dict, path: str) -> list[str]:
    if spec["prefixes"] == "Quad":
        return [prefix + path for prefix in QUAD_PREFIXES]
    if spec["prefixes"] == "Internal":
        return ["/internal/" + path]
    return [path]


def primary_url(spec: dict) -> str:
    return urls_for(spec, spec["paths"][0])[-1]


def auth_sentence(url: str) -> str:
    # Mirrors RequestMiddleware::pre_route(), which decides auth by path.
    if url.startswith("/internal/"):
        return (
            "Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise "
            "`LEMONADE_API_KEY` when that is set."
        )
    if url.startswith(("/api/", "/v0/", "/v1/")) or url in ("/mcp", "/metrics"):
        return "Requires `LEMONADE_API_KEY` when it is set."
    return "Requires no API key."


def setup_chain(specs: dict, ref: dict, seen: tuple = ()) -> list[dict]:
    """Every step that must run before example ref, in order, each named example after
    its own setup."""
    example = find_example(specs, ref)
    chain = []
    for step in example["setup"]:
        if step["args"] is not None:
            chain.append(step)
            continue
        key = (step["route"], step["example"])
        if key in seen:
            raise GenerationError(f"{ref['route']}: setup cycle through {key}")
        chain += setup_chain(specs, step, seen + (key,))
        chain.append(step)
    return chain


def find_example(specs: dict, ref: dict) -> dict:
    spec = specs.get(ref["route"])
    if spec is None:
        raise GenerationError(f"setup names unknown route {ref['route']}")
    for example in spec["examples"]:
        if example["name"] == ref["example"]:
            return example
    raise GenerationError(f"{ref['route']} has no example named {ref['example']!r}")


def find_response(spec: dict, fmt: str) -> dict:
    for response in spec["responses"]:
        if response["format"] == fmt:
            return response
    raise GenerationError(f"{spec['id']} has no {fmt} response")


def step_payload(specs: dict, step: dict) -> dict:
    """A named step hashes as the example it runs, an inline step as its request."""
    if step["args"] is not None:
        return {"route": step["route"], "args": step["args"]}
    example = find_example(specs, step)
    return {
        "route": step["route"],
        "format": example["format"],
        "example": example["args"],
    }


def pinned_backend(examples: list[dict], backend_versions: dict, server_models: dict):
    """A pass-through route's response depends on the backend serving its model, which
    its own example names, or else the setup example that loads it."""
    for example in examples:
        model = example.get("model", example.get("model_name"))
        if isinstance(model, str):
            recipe = server_models.get(model, {}).get("recipe")
            return backend_versions.get(recipe) if recipe else None
    return None


def cache_key(
    specs: dict,
    route_id: str,
    example: dict,
    backend_versions: dict,
    server_models: dict,
) -> str:
    ref = {"route": route_id, "example": example["name"]}
    setup = [step_payload(specs, step) for step in setup_chain(specs, ref)]
    requests = [example["args"]] + [
        entry.get("example", entry.get("args")) for entry in reversed(setup)
    ]
    payload = {
        "route": route_id,
        "format": example["format"],
        "example": example["args"],
        "setup": setup,
        "backend": pinned_backend(requests, backend_versions, server_models),
    }
    canonical = json.dumps(payload, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def validate_specs(specs: dict) -> None:
    for route_id, spec in specs.items():
        if route_id.endswith(".summary"):
            raise GenerationError(
                f"route id {route_id} collides with the summary region"
            )
        for response in spec["responses"]:
            where = f"{route_id} {response['format']}"
            has_schema = bool(response["schema"])
            if response["format"] in JSON_FORMATS and not has_schema:
                raise GenerationError(f"{where}: a JSON format needs a schema")
            if response["format"] not in JSON_FORMATS and has_schema:
                raise GenerationError(f"{where}: only JSON formats have a schema")
        names = [example["name"] for example in spec["examples"]]
        if len(names) > 1 and (not all(names) or len(set(names)) != len(names)):
            raise GenerationError(
                f"{route_id}: several examples need distinct names, got {names}"
            )
        for example in spec["examples"]:
            where = f"{route_id} example {example['name']!r}"
            find_response(spec, example["format"])
            check_args(specs, route_id, example["args"], where)
            for step in example["setup"]:
                if step["args"] is not None:
                    check_args(specs, step["route"], step["args"], where)
            chain_routes = {
                step["route"]
                for step in setup_chain(
                    specs, {"route": route_id, "example": example["name"]}
                )
            }
            for value in iter_strings(example["args"]):
                if value.startswith("$"):
                    target = value[1:].split("/", 1)[0]
                    if target not in chain_routes:
                        raise GenerationError(
                            f"{where}: {value} names a route that does not run in its setup"
                        )


def check_args(specs: dict, route_id: str, values, where: str) -> None:
    spec = specs.get(route_id)
    if spec is None:
        raise GenerationError(f"{where}: setup names unknown route {route_id}")
    if not isinstance(values, dict):
        raise GenerationError(f"{where}: args must be an object of arguments")
    args = {arg["name"] for arg in spec["args"]}
    for name in values:
        if name not in args:
            raise GenerationError(f"{where}: names unknown argument {route_id} {name}")
    for value in iter_strings(values):
        if value.startswith("@fixtures/") and not (FIXTURES / value[10:]).exists():
            raise GenerationError(f"{where}: missing fixture {value}")


def iter_strings(value):
    if isinstance(value, str):
        yield value
    elif isinstance(value, dict):
        for item in value.values():
            yield from iter_strings(item)
    elif isinstance(value, list):
        for item in value:
            yield from iter_strings(item)


# --------------------------------------------------------------------------
# Route text lint (the spec's Route Documentation Contract)
# --------------------------------------------------------------------------

GENERATED_PHRASES = ["Also served at", "Requires `LEMONADE_API_KEY`", "> **Note:**"]
OTHER_PREFIXES = re.compile(r"/api/v[01]\b|(?<![\w/])/v0/")
PLACEHOLDER = re.compile(r"<(?!(?:sub|br)\b)[A-Za-z_][\w-]*>")
SENTENCE_END = re.compile(r"(?<!e\.g)(?<!i\.e)(?<=[.!?])\s+")
MAX_SUMMARY_WORDS = 10
MAX_DESCRIPTION_SENTENCES = 3
REPEAT_MIN_CHARS = 40


def without_code(text: str) -> str:
    return re.sub(r"`[^`]*`", "", re.sub(r"```.*?```", "", text, flags=re.S))


def sentences(text: str) -> list[str]:
    return [part for part in SENTENCE_END.split(without_code(text).strip()) if part]


def route_texts(spec: dict):
    yield "summary", spec["summary"]
    yield "description", spec["description"]
    for index, note in enumerate(spec["notes"]):
        yield f"notes[{index}]", note
    for arg in spec["args"]:
        yield f"argument {arg['name']}", arg["description"]

    def walk(schema, path):
        if not isinstance(schema, dict):
            return
        if isinstance(schema.get("description"), str):
            yield f"field {path or '(root)'}", schema["description"]
        for key, value in schema.get("properties", {}).items():
            yield from walk(value, f"{path}.{key}" if path else key)
        for key in ("items", "additionalProperties"):
            yield from walk(schema.get(key), path)
        for key in ("oneOf", "anyOf", "allOf"):
            for branch in schema.get(key, []):
                yield from walk(branch, path)

    for response in spec["responses"]:
        yield from walk(response["schema"], "")


def lint_spec(spec: dict) -> list[str]:
    problems = []
    for where, text in route_texts(spec):
        prose = without_code(text)
        for phrase in GENERATED_PHRASES:
            if phrase in text:
                problems.append(f"{where} restates generated text: {phrase!r}")
        if OTHER_PREFIXES.search(text):
            problems.append(f"{where} names a prefix other than /v1/")
        if "\u2014" in text:
            problems.append(f"{where} has an em dash")
        for match in PLACEHOLDER.finditer(prose):
            problems.append(f"{where} has {match.group(0)} outside backticks")
    if len(spec["summary"].split()) > MAX_SUMMARY_WORDS:
        problems.append(f"summary runs over {MAX_SUMMARY_WORDS} words")
    if len(sentences(spec["description"])) > MAX_DESCRIPTION_SENTENCES:
        problems.append(f"description runs over {MAX_DESCRIPTION_SENTENCES} sentences")
    return [f"{spec['id']}: {problem}" for problem in problems]


def lint_region(route_id: str, body: str) -> list[str]:
    """A sentence repeated within one route's region. Field lists and tables are left out,
    since tabs for different formats list their shared fields each."""
    lines = [
        line.strip()
        for line in re.sub(r"```.*?```", "", body, flags=re.S).split("\n")
        if not line.strip().startswith(("- `", "|", "===", "#"))
    ]
    counts = collections.Counter(
        part for part in sentences(" ".join(lines)) if len(part) >= REPEAT_MIN_CHARS
    )
    return [
        f"{route_id}: repeats {sentence[:80]!r}"
        for sentence, count in counts.items()
        if count > 1
    ]


# --------------------------------------------------------------------------
# Running examples
# --------------------------------------------------------------------------


def auth_headers() -> dict:
    key = os.environ.get("LEMONADE_ADMIN_API_KEY") or os.environ.get("LEMONADE_API_KEY")
    return {"Authorization": f"Bearer {key}"} if key else {}


class ExampleRunner:
    """Runs examples against one lemond, restarting it after an example stops it."""

    def __init__(self, binary: Path, bin_dir: str | None, specs: dict):
        self.binary = binary
        self.bin_dir = bin_dir
        self.specs = specs
        self.server: Lemond | None = None
        self.last_body: dict[str, object] = {}

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        if self.server:
            self.server.__exit__(None, None, None)

    def start(self):
        self.server = Lemond(self.binary, bin_dir=self.bin_dir).__enter__()

    def ensure_running(self):
        if self.server._proc.poll() is not None:
            self.server.__exit__(None, None, None)
            self.start()

    def resolve(self, value):
        if isinstance(value, str) and value.startswith("$"):
            route_id, _, pointer = value[1:].partition("/")
            body = self.last_body.get(route_id)
            for part in pointer.split("/") if pointer else []:
                body = body[int(part)] if isinstance(body, list) else body[part]
            return body
        if isinstance(value, dict):
            return {key: self.resolve(item) for key, item in value.items()}
        if isinstance(value, list):
            return [self.resolve(item) for item in value]
        return value

    def build_request(self, spec: dict, example: dict) -> dict:
        args = {arg["name"]: arg for arg in spec["args"]}
        request = {
            "method": spec["methods"][0],
            "path": primary_url(spec),
            "query": {},
            "json": None,
            "form": {},
            "files": {},
        }
        resolved = self.resolve(example)
        for name, value in resolved.items():
            where = args[name]["in"]
            if where == "Path":
                pattern = re.compile(r"\{" + re.escape(name) + r"\}")
                request["path"] = pattern.sub(str(value), request["path"])
            elif where == "Query":
                request["query"][name] = value
            elif where == "Form":
                if isinstance(value, str) and value.startswith("@fixtures/"):
                    request["files"][name] = value
                else:
                    request["form"][name] = value
            else:
                if request["json"] is None:
                    request["json"] = {}
                request["json"][name] = value
        return request

    def send(self, request: dict, response_format: str) -> dict:
        url = f"http://127.0.0.1:{self.server.port}{request['path']}"
        if request["query"]:
            url += "?" + urllib.parse.urlencode(
                {k: json_scalar(v) for k, v in request["query"].items()}
            )
        headers = auth_headers()
        data = None
        if request["files"] or request["form"]:
            boundary = uuid.uuid4().hex
            data = multipart_body(boundary, request["form"], request["files"])
            headers["Content-Type"] = f"multipart/form-data; boundary={boundary}"
        elif request["json"] is not None:
            data = json.dumps(embed_fixtures(request["json"])).encode("utf-8")
            headers["Content-Type"] = "application/json"
        elif request["method"] in ("POST", "PUT"):
            data = b""
        http_request = urllib.request.Request(
            url, data=data, headers=headers, method=request["method"]
        )
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        try:
            with opener.open(http_request, timeout=EXAMPLE_TIMEOUT) as response:
                status = response.status
                content_type = response.headers.get("Content-Type", "")
                raw = response.read()
        except urllib.error.HTTPError as error:
            status = error.code
            content_type = error.headers.get("Content-Type", "")
            raw = error.read()
        except (ConnectionError, urllib.error.URLError) as error:
            # /internal/shutdown answers before it exits, but may close the
            # connection first.
            if request["path"] != "/internal/shutdown":
                raise
            status, content_type, raw = (
                200,
                "application/json",
                b'{"status":"shutting down"}',
            )
            del error
        return record(
            status, content_type, raw, response_format, self.server._cache.name
        )

    def run(self, route_id: str, name: str) -> dict:
        """Runs one example after its setup, returning its cache entry."""
        ref = {"route": route_id, "example": name}
        for step in setup_chain(self.specs, ref):
            if step["args"] is not None:
                self.run_one(step["route"], step["args"], None, "setup")
            else:
                example = find_example(self.specs, step)
                self.run_one(
                    step["route"], example["args"], example["format"], step["example"]
                )
        example = find_example(self.specs, ref)
        return self.run_one(route_id, example["args"], example["format"], name)

    def run_one(
        self, route_id: str, values: dict, response_format: str | None, name: str
    ) -> dict:
        spec = self.specs[route_id]
        self.ensure_running()
        request = self.build_request(spec, values)
        print(f"  running {route_id} {name or response_format} ...", flush=True)
        recorded = self.send(request, response_format or "Json")
        if recorded["status"] >= 300:
            raise GenerationError(
                f"{route_id} {name or response_format} example answered {recorded['status']}: "
                f"{json.dumps(recorded)[:500]}"
            )
        if "body" in recorded:
            self.last_body[route_id] = recorded["body"]
        if request["path"] == "/internal/shutdown":
            deadline = time.time() + 30
            while self.server._proc.poll() is None and time.time() < deadline:
                time.sleep(0.2)
        return {
            "route": route_id,
            "format": response_format,
            "request": request,
            "response": recorded,
        }


def json_scalar(value) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


def embed_fixtures(value):
    if isinstance(value, str) and value.startswith("@fixtures/"):
        return base64.b64encode((FIXTURES / value[10:]).read_bytes()).decode("ascii")
    if isinstance(value, dict):
        return {key: embed_fixtures(item) for key, item in value.items()}
    if isinstance(value, list):
        return [embed_fixtures(item) for item in value]
    return value


def multipart_body(boundary: str, fields: dict, files: dict) -> bytes:
    parts = []
    for name, value in fields.items():
        parts.append(
            f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"\r\n\r\n'
            f"{json_scalar(value)}\r\n".encode("utf-8")
        )
    for name, value in files.items():
        filename = value[10:]
        parts.append(
            f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"; '
            f'filename="{filename}"\r\nContent-Type: application/octet-stream\r\n\r\n'.encode()
            + (FIXTURES / filename).read_bytes()
            + b"\r\n"
        )
    parts.append(f"--{boundary}--\r\n".encode())
    return b"".join(parts)


def redact(text: str, cache_dir: str) -> str:
    """Recorded responses must not carry the recording machine's home directory, and show
    the harness's temporary cache directory as the default one."""
    replacements = [(cache_dir, "~/.cache/lemonade")]
    home = str(Path.home())
    if home and home != "/":
        replacements.append((home, "~"))
    for path, shown in replacements:
        # JSON escapes the backslashes of a Windows path.
        text = text.replace(json.dumps(path)[1:-1], shown).replace(path, shown)
    return text


def elide(value):
    if isinstance(value, str) and len(value) > LONG_STRING:
        return f"{value[:LONG_STRING_KEPT]}... ({len(value):,} characters)"
    if isinstance(value, dict):
        return {key: elide(item) for key, item in value.items()}
    if isinstance(value, list):
        return [elide(item) for item in value]
    return value


def elide_json_text(text: str) -> str:
    """Shortens the long strings of one NDJSON line or SSE data payload."""
    try:
        return json.dumps(
            elide(json.loads(text)), ensure_ascii=False, separators=(",", ":")
        )
    except json.JSONDecodeError:
        return text


def elide_event(event: str) -> str:
    return "\n".join(
        (
            "data: " + elide_json_text(line[5:].strip())
            if line.startswith("data:")
            else line
        )
        for line in event.splitlines()
    )


def split_events(text: str) -> list[str]:
    events = [block.strip("\r\n") for block in re.split(r"\r?\n\r?\n", text)]
    return [event for event in events if event.strip()]


def truncate(items: list) -> dict:
    if len(items) <= STREAM_HEAD + STREAM_TAIL:
        return {"head": items, "tail": [], "count": len(items)}
    return {
        "head": items[:STREAM_HEAD],
        "tail": items[-STREAM_TAIL:],
        "count": len(items),
    }


def record(
    status: int, content_type: str, raw: bytes, response_format: str, cache_dir: str
) -> dict:
    recorded = {"status": status, "content_type": content_type}
    if response_format == "Json":
        recorded["body"] = elide(json.loads(redact(raw.decode("utf-8"), cache_dir)))
    elif response_format == "JsonLines":
        lines = [
            line
            for line in redact(raw.decode("utf-8"), cache_dir).splitlines()
            if line.strip()
        ]
        recorded.update(truncate([elide_json_text(line) for line in lines]))
    elif response_format == "EventStream":
        events = split_events(redact(raw.decode("utf-8"), cache_dir))
        recorded.update(truncate([elide_event(event) for event in events]))
    elif response_format == "Text":
        lines = redact(raw.decode("utf-8"), cache_dir).splitlines()
        recorded["lines"] = lines[:TEXT_LINES]
        recorded["line_count"] = len(lines)
    elif response_format in ("Binary", "BinaryStream"):
        recorded["bytes"] = len(raw)
    return recorded


# --------------------------------------------------------------------------
# Checking recorded responses
# --------------------------------------------------------------------------


def event_payloads(event: str) -> list[str]:
    data_lines = [
        line[5:].lstrip() for line in event.splitlines() if line.startswith("data:")
    ]
    if not data_lines:
        raise GenerationError(f"event has no data line: {event[:200]}")
    return ["\n".join(data_lines)]


def check_entry(spec: dict, response: dict, entry: dict) -> None:
    import jsonschema

    where = f"{spec['id']} {response['format']}"
    recorded = entry["response"]
    fmt = response["format"]
    validator = (
        jsonschema.Draft202012Validator(response["schema"])
        if response["schema"]
        else None
    )

    def validate(value):
        errors = sorted(validator.iter_errors(value), key=lambda e: list(e.path))
        if errors:
            error = errors[0]
            path = "/".join(str(part) for part in error.path)
            raise GenerationError(
                f"{where}: recorded response fails its schema at "
                f"'{path}': {error.message[:300]}"
            )

    if fmt == "Json":
        if "body" not in recorded:
            raise GenerationError(f"{where}: recorded response has no JSON body")
        validate(recorded["body"])
    elif fmt in ("JsonLines", "EventStream"):
        items = recorded.get("head", []) + recorded.get("tail", [])
        if not items:
            raise GenerationError(f"{where}: recorded stream is empty")
        for item in items:
            payloads = [item] if fmt == "JsonLines" else event_payloads(item)
            for payload in payloads:
                if payload == "[DONE]":
                    continue
                try:
                    validate(json.loads(payload))
                except json.JSONDecodeError as error:
                    raise GenerationError(
                        f"{where}: not JSON: {payload[:200]}"
                    ) from error
    elif fmt == "Text":
        if "lines" not in recorded:
            raise GenerationError(f"{where}: recorded response has no text")
    elif fmt in ("Binary", "BinaryStream"):
        if not recorded.get("bytes"):
            raise GenerationError(f"{where}: recorded response has no bytes")
    elif fmt == "Empty":
        if any(key in recorded for key in ("body", "head", "lines", "bytes")):
            raise GenerationError(f"{where}: recorded response is not empty")


# --------------------------------------------------------------------------
# Rendering
# --------------------------------------------------------------------------


def md_cell(text: str) -> str:
    return text.replace("|", "\\|").replace("\n", " ")


def anchor(heading: str) -> str:
    text = heading.replace("`", "").lower()
    text = re.sub(r"[^\w\s-]", "", text)
    return re.sub(r"\s+", "-", text.strip())


def heading_of(spec: dict) -> str:
    return f"`{', '.join(spec['methods'])} {primary_url(spec)}`"


def route_status(spec: dict) -> str:
    if spec["experimental"]:
        return "experimental-orange"
    if not spec["responses"]:
        return "not_available-red"
    if all(arg["supported"] == "available" for arg in spec["args"]):
        return "fully_available-green"
    return "partially_available-yellow"


ARG_BADGES = {
    "available": "available-green",
    "partial": "partial-yellow",
    "not_available": "not_available-red",
}


def oxford(items: list[str]) -> str:
    if len(items) == 1:
        return items[0]
    return ", ".join(items[:-1]) + " and " + items[-1]


def indent(text: str, prefix: str = "    ") -> str:
    return "\n".join(prefix + line if line else "" for line in text.splitlines())


def fence(language: str, body: str) -> str:
    return f"```{language}\n{body}\n```"


def json_layout(value, width: int = 96, column: int = 0, level: int = 0) -> str:
    """Indented JSON that keeps any value fitting the line on that line.

    A long array of numbers or strings that does not fit, such as an embedding, shows
    its first items and how many more there are, so the docs stay readable.
    """
    compact = json.dumps(value, ensure_ascii=False)
    if (
        not isinstance(value, (dict, list))
        or not value
        or column + len(compact) <= width
    ):
        return compact
    if (
        isinstance(value, list)
        and len(value) > LONG_ARRAY
        and not any(isinstance(item, (dict, list)) for item in value)
    ):
        shown = ", ".join(json.dumps(item, ensure_ascii=False) for item in value[:8])
        return f"[{shown}, ... {len(value) - 8} more]"
    pad = "  " * level
    inner = "  " * (level + 1)
    if isinstance(value, dict):
        items = []
        for key, item in value.items():
            prefix = f"{inner}{json.dumps(key, ensure_ascii=False)}: "
            items.append(prefix + json_layout(item, width, len(prefix), level + 1))
        return "{\n" + ",\n".join(items) + f"\n{pad}}}"
    items = [inner + json_layout(item, width, len(inner), level + 1) for item in value]
    return "[\n" + ",\n".join(items) + f"\n{pad}]"


def fixture_variables(value) -> dict[str, str]:
    """Fixture files in a JSON body, by the shell variable that holds their base64."""
    names = {}
    for item in iter_strings(value):
        if item.startswith("@fixtures/"):
            filename = item[10:]
            names[item] = re.sub(r"\W", "_", Path(filename).stem)
    return names


def request_url(request: dict) -> str:
    url = DOCS_HOST + request["path"]
    if request["query"]:
        url += "?" + "&".join(
            f"{k}={json_scalar(v)}" for k, v in request["query"].items()
        )
    return url


def output_file(entry: dict) -> str | None:
    content_type = entry["response"]["content_type"].split(";")[0].strip()
    if "bytes" not in entry["response"]:
        return None
    extension = BINARY_EXTENSIONS.get(
        content_type, content_type.split("/")[-1] or "bin"
    )
    return f"output.{extension}"


def body_text(value) -> str:
    """JSON for a request body: one line when short, else indented under the command."""
    return json_layout(value, width=88).replace("\n", "\n    ")


def bash_command(entry: dict) -> str:
    request = entry["request"]
    url = request_url(request)
    quoted_url = f'"{url}"' if "?" in url else url
    has_body = request["json"] is not None or request["form"] or request["files"]
    if request["method"] == "GET" or (request["method"] == "POST" and has_body):
        lines = [f"curl {quoted_url}"]
    else:
        lines = [f"curl -X {request['method']} {quoted_url}"]
    if request["json"] is not None:
        lines.append('-H "Content-Type: application/json"')
        variables = fixture_variables(request["json"])
        body = body_text(request["json"])
        if variables:
            body = body.replace('"', '\\"')
            for marker in variables:
                body = body.replace(marker, f"$(base64 -w0 {marker[10:]})")
            lines.append(f'-d "{body}"')
        else:
            lines.append("-d '" + body.replace("'", "'\\''") + "'")
    for name, value in request["form"].items():
        lines.append(f'-F "{name}={json_scalar(value)}"')
    for name, value in request["files"].items():
        lines.append(f"-F {name}=@{value[10:]}")
    if output_file(entry):
        lines.append(f"--output {output_file(entry)}")
    return " \\\n  ".join(lines)


def powershell_command(entry: dict) -> str:
    request = entry["request"]
    if request["form"] or request["files"]:
        # Invoke-WebRequest -Form needs PowerShell 7; Windows ships curl.exe.
        lines = [f"curl.exe {request_url(request)}"]
        lines += [
            f'-F "{name}={json_scalar(value)}"'
            for name, value in request["form"].items()
        ]
        lines += [
            f"-F {name}=@{value[10:]}" for name, value in request["files"].items()
        ]
        if output_file(entry):
            lines.append(f"--output {output_file(entry)}")
        return " `\n  ".join(lines)
    prelude = []
    lines = ["Invoke-WebRequest", f'-Uri "{request_url(request)}"']
    if request["method"] != "GET":
        lines.append(f"-Method {request['method']}")
    if request["json"] is not None:
        lines.append('-Headers @{ "Content-Type" = "application/json" }')
        variables = fixture_variables(request["json"])
        body = body_text(request["json"])
        if variables:
            for marker, name in variables.items():
                prelude.append(
                    f"${name} = [Convert]::ToBase64String("
                    f'[IO.File]::ReadAllBytes("{marker[10:]}"))'
                )
                body = body.replace(marker, f"${name}")
            prelude.append(f'$body = @"\n{body}\n"@')
            lines.append("-Body $body")
        else:
            lines.append("-Body '" + body.replace("'", "''") + "'")
    if output_file(entry):
        lines.append(f"-OutFile {output_file(entry)}")
    if len(lines) == 2:
        return "\n".join(prelude + [" ".join(lines)])
    return "\n".join(prelude + [" `\n  ".join(lines)])


def dereferenced(schema, root=None):
    """The schema with each $ref inlined, resolved within its enclosing $id resource."""
    if isinstance(schema, list):
        return [dereferenced(item, root) for item in schema]
    if not isinstance(schema, dict):
        return schema
    if root is None or "$id" in schema:
        root = schema
    if "$ref" in schema:
        target = root
        for part in schema["$ref"].removeprefix("#/").split("/"):
            target = target[part]
        rest = {key: value for key, value in schema.items() if key != "$ref"}
        return dereferenced({**target, **rest}, root)
    return {key: dereferenced(value, root) for key, value in schema.items()}


def schema_branches(schema) -> list[dict]:
    """The schema, then each of its oneOf, anyOf and allOf alternatives."""
    if not isinstance(schema, dict):
        return []
    branches = [schema]
    for key in ("oneOf", "anyOf", "allOf"):
        for alternative in schema.get(key, []):
            branches += schema_branches(alternative)
    return branches


def properties_of(schema) -> dict:
    """Every property a value of this schema can have, in schema order; an array's are its items'."""
    merged = {}
    for branch in schema_branches(schema):
        if "items" in branch:
            for name, sub in properties_of(branch["items"]).items():
                merged.setdefault(name, sub)
        for name, sub in branch.get("properties", {}).items():
            merged.setdefault(name, sub)
    return merged


def map_entry_of(schema):
    """The schema of each entry of a map, such as system-info's recipes by name."""
    return next(
        (
            b["additionalProperties"]
            for b in schema_branches(schema)
            if isinstance(b.get("additionalProperties"), dict)
        ),
        None,
    )


def description_of(schema) -> str:
    return next(
        (b["description"] for b in schema_branches(schema) if "description" in b),
        "",
    )


def field_rows(schema) -> list[tuple[list[str], str, list]]:
    """Described fields as (names, description, nested rows).

    Adjacent fields sharing a description share a row, as in `file`, `file_index`,
    `total_files`. A described map entry gets a row with no names, as in system-info's
    "Each recipe ... contains:". A nested schema with its own $id, such as the routing
    decision, is documented on its own page, so only its description is listed.
    """
    rows = []
    for name, sub in properties_of(schema).items():
        description = description_of(sub)
        children = [] if "$id" in sub else field_rows(sub)
        if not description and not children:
            continue
        previous = rows[-1] if rows else None
        if previous and description == previous[1] and not children and not previous[2]:
            previous[0].append(name)
        else:
            rows.append(([name], description, children))
    entry = map_entry_of(schema)
    if entry is not None and description_of(entry):
        rows.append(([], description_of(entry), field_rows(entry)))
    return rows


def field_lines(rows: list, pad: str = "") -> list[str]:
    lines = []
    for names, description, children in rows:
        if not names:
            lines.append(f"{pad}- {description}")
        else:
            line = pad + "- " + ", ".join(f"`{name}`" for name in names)
            lines.append(line + (f" - {description}" if description else ""))
        lines += field_lines(children, pad + "  ")
    return lines


def render_fields(schema: dict) -> str:
    rows = field_rows(schema)
    if not rows:
        return ""
    return "**Field Descriptions:**\n\n" + "\n".join(field_lines(rows))


def in_schema_order(value, schema):
    """lemond answers with sorted keys; list each object's fields in its schema's order."""
    if isinstance(value, list):
        items = next((b["items"] for b in schema_branches(schema) if "items" in b), {})
        return [in_schema_order(item, items) for item in value]
    if not isinstance(value, dict):
        return value
    properties = properties_of(schema)
    entry = map_entry_of(schema) or {}
    rank = {name: index for index, name in enumerate(properties)}
    return {
        key: in_schema_order(value[key], properties.get(key, entry))
        for key in sorted(value, key=lambda k: rank.get(k, len(rank)))
    }


def ordered_payload(text: str, schema: dict) -> str:
    try:
        payload = json.loads(text)
    except json.JSONDecodeError:
        return text
    return json.dumps(in_schema_order(payload, schema), ensure_ascii=False)


def render_event(event: str, schema: dict) -> str:
    rendered = []
    for line in event.splitlines():
        if line.startswith("data:"):
            rendered.append(f"data: {ordered_payload(line[5:].strip(), schema)}")
        else:
            rendered.append(line)
    return "\n".join(rendered)


def render_recorded(fmt: str, entry: dict, schema: dict) -> str:
    recorded = entry["response"]
    status = f"`{recorded['status']}`"
    if fmt == "Json":
        body = in_schema_order(recorded["body"], schema)
        return status + "\n\n" + fence("json", json_layout(body))
    if fmt in ("JsonLines", "EventStream"):
        render = (
            (lambda line: ordered_payload(line, schema))
            if fmt == "JsonLines"
            else (lambda event: render_event(event, schema))
        )
        shown = [render(item) for item in recorded["head"]]
        if recorded["tail"]:
            shown.append("...")
            shown += [render(item) for item in recorded["tail"]]
        return status + "\n\n" + fence("text", "\n".join(shown))
    if fmt == "Text":
        lines = list(recorded["lines"])
        if recorded["line_count"] > len(lines):
            lines.append("...")
        return status + "\n\n" + fence("text", "\n".join(lines))
    if fmt in ("Binary", "BinaryStream"):
        return (
            f"{status}, `Content-Type: {recorded['content_type']}`, "
            f"{recorded['bytes']:,} bytes"
        )
    return status


def render_tab(title: str, body: str) -> str:
    return f'=== "{title}"\n\n{indent(body)}\n'


def like(template, value):
    """value with each object's keys in the order template lists them."""
    if isinstance(template, dict) and isinstance(value, dict):
        rank = {key: index for index, key in enumerate(template)}
        return {
            key: like(template.get(key), value[key])
            for key in sorted(value, key=lambda k: rank.get(k, len(rank)))
        }
    if isinstance(template, list) and isinstance(value, list):
        return [like(t, v) for t, v in zip(template, value)] + value[len(template) :]
    return value


def in_example_order(example: dict, entry: dict) -> dict:
    """A recorded request keeps the key order it was recorded with; show the example's."""
    request = entry["request"]
    reordered = {
        key: like(example, request[key]) for key in ("json", "query", "form", "files")
    }
    return {**entry, "request": {**request, **reordered}}


def render_example_request(entries: list[dict], labels: list[str]) -> str:
    parts = ["### Example request\n"]
    for index, (entry, label) in enumerate(zip(entries, labels)):
        suffix = f" ({label})" if index else ""
        parts.append(
            render_tab(
                f"PowerShell{suffix}", fence("powershell", powershell_command(entry))
            )
        )
        parts.append(render_tab(f"Bash{suffix}", fence("bash", bash_command(entry))))
    return "\n".join(parts).rstrip()


def render_response_format(spec: dict, entries: list[dict], labels: list[str]) -> str:
    """Each format's Field Descriptions appear once: after the tabs when every example
    shares one format, else inside each example's tab."""
    shared = len({example["format"] for example in spec["examples"]}) == 1
    bodies, fields = [], ""
    for example, entry in zip(spec["examples"], entries):
        response = find_response(spec, example["format"])
        schema = dereferenced(response["schema"])
        body = render_recorded(response["format"], entry, schema)
        fields = render_fields(schema) if schema else ""
        bodies.append(body if shared or not fields else f"{body}\n\n{fields}")
    if len(bodies) == 1:
        return (
            "### Response format\n\n" + bodies[0] + (f"\n\n{fields}" if fields else "")
        )
    parts = ["### Response format\n"]
    for label, body in zip(labels, bodies):
        parts.append(render_tab(f"{label[0].upper()}{label[1:]} responses", body))
    if shared and fields:
        parts.append(fields)
    return "\n".join(parts).rstrip()


def render_parameters(spec: dict) -> str:
    if not spec["args"]:
        return "### Parameters\n\nThis endpoint does not take any parameters."
    rows = [
        "### Parameters",
        "",
        "| Parameter | Required | Description | Status |",
        "|-----------|----------|-------------|--------|",
    ]
    for arg in spec["args"]:
        name = f"`{arg['name']}`"
        if arg["in"] == "Query":
            name += " (query)"
        elif arg["in"] == "Path":
            name += " (path)"
        status = BADGE.format(ARG_BADGES[arg["supported"]])
        rows.append(
            f"| {name} | {'Yes' if arg['required'] else 'No'} | "
            f"{md_cell(arg['description'])} | {status} |"
        )
    return "\n".join(rows)


def render_route(spec: dict, entries: dict) -> str:
    # A Quad route is documented only at /v1/; its other prefixes serve existing clients.
    others = [urls_for(spec, path)[-1] for path in spec["paths"][1:]]
    served = (
        f"Also served at {oxford([f'`{url}`' for url in others])}. " if others else ""
    )

    parts = [
        f"## {heading_of(spec)}",
        BADGE.format("status-" + route_status(spec)),
        "",
        spec["description"],
        "",
        served + auth_sentence(primary_url(spec)),
    ]
    if spec["responses"]:
        parts += ["", render_parameters(spec)]
    for note in spec["notes"]:
        parts += ["", note]
    if spec["responses"]:
        ordered = [
            in_example_order(example["args"], entries[example["name"]])
            for example in spec["examples"]
        ]
        labels = [example["name"] for example in spec["examples"]]
        parts += [
            "",
            render_example_request(ordered, labels),
            "",
            render_response_format(spec, ordered, labels),
        ]
    return "\n".join(parts)


def render_summary(page_specs: list[dict], current: str, page_text: str) -> str:
    """Rows linking to a route's section are generated from its summary, in the order the
    sections appear on the page. Other rows, such as a WebSocket endpoint's, are
    hand-written and stay after the row they follow."""
    with_status = any(not spec["responses"] for spec in page_specs)
    with_modality = any(spec["modality"] for spec in page_specs)
    columns = (
        ["Method", "Endpoint"]
        + ["Status"] * with_status
        + ["Description"]
        + ["Modality"] * with_modality
    )
    generated = {}
    for spec in page_specs:
        if not spec["summary"]:
            continue
        target = anchor(heading_of(spec))
        cells = (
            [
                ", ".join(f"`{method}`" for method in spec["methods"]),
                f"[`{primary_url(spec)}`](#{target})",
            ]
            + ["Supported" if spec["responses"] else "Not supported"] * with_status
            + [md_cell(spec["summary"])]
            + [md_cell(spec["modality"])] * with_modality
        )
        generated[target] = f"| {' | '.join(cells)} |"
    headings = [
        anchor(line.lstrip("#").strip())
        for line in page_text.splitlines()
        if line.startswith("#")
    ]
    sections = set(headings)
    position = {target: index for index, target in reversed(list(enumerate(headings)))}
    kept: dict[str | None, list[str]] = {}
    previous = None
    for line in current.splitlines()[2:]:
        match = re.search(r"\]\(#([^)]+)\)", line)
        target = match.group(1) if match else None
        if target in generated:
            previous = target
            continue
        if target not in sections:
            raise GenerationError(
                f"summary row links to no section on the page: {line}"
            )
        kept.setdefault(previous, []).append(line)
    rows = [
        f"| {' | '.join(columns)} |",
        f"|{'|'.join('-' * (len(column) + 2) for column in columns)}|",
    ] + kept.get(None, [])
    for target in sorted(generated, key=lambda t: position.get(t, len(position))):
        rows.append(generated[target])
        rows += kept.get(target, [])
    table = "\n".join(rows)
    for spec in page_specs:
        if f"](#{anchor(heading_of(spec))})" not in table:
            raise GenerationError(
                f"{spec['id']} has no summary and no hand-written row"
            )
    return table


# --------------------------------------------------------------------------
# Pages
# --------------------------------------------------------------------------

REGION_RE = re.compile(
    r"<!-- BEGIN GENERATED: (\S+) -->\n(.*?)<!-- END GENERATED: \1 -->", re.DOTALL
)


def region(region_id: str, body: str) -> str:
    return f"<!-- BEGIN GENERATED: {region_id} -->\n{body}\n<!-- END GENERATED: {region_id} -->"


def update_page(
    text: str, page: str, page_specs: list[dict], rendered: dict[str, str], check: bool
) -> tuple[str, list[str]]:
    """Rewrites a page's regions; returns the new text and its structural problems."""
    problems = []
    wanted = [f"{page}.summary"] + [spec["id"] for spec in page_specs]
    # Other generators own regions whose ids are not this page's, such as mcp-tools.
    present = [
        match.group(1)
        for match in REGION_RE.finditer(text)
        if match.group(1).startswith(page + ".")
    ]
    for region_id in present:
        if region_id not in wanted:
            problems.append(f"region {region_id} has no route")
            text = REGION_RE.sub(
                lambda m: "" if m.group(1) == region_id else m.group(0), text
            )
    for index, region_id in enumerate(wanted):
        if region_id in present:
            continue
        problems.append(f"route {region_id} has no region")
        later = [
            rid for rid in wanted[index + 1 :] if f"BEGIN GENERATED: {rid} -->" in text
        ]
        block = region(region_id, "") + "\n\n"
        if later:
            marker = f"<!-- BEGIN GENERATED: {later[0]} -->"
            text = text.replace(marker, block + marker, 1)
        else:
            text = text.rstrip("\n") + "\n\n" + block
    text = REGION_RE.sub(
        lambda m: (
            region(m.group(1), rendered[m.group(1)])
            if m.group(1) in rendered
            else m.group(0)
        ),
        text,
    )
    return text, problems


# --------------------------------------------------------------------------
# Main
# --------------------------------------------------------------------------


def load_cache() -> dict:
    if not CACHE_FILE.exists():
        return {}
    return json.loads(CACHE_FILE.read_text(encoding="utf-8"))


def write_cache(cache: dict) -> None:
    ordered = dict(
        sorted(
            cache.items(),
            key=lambda kv: (
                kv[1]["route"],
                kv[1]["format"],
                json.dumps(kv[1]["request"], sort_keys=True),
            ),
        )
    )
    CACHE_FILE.write_text(
        json.dumps(ordered, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
        newline="\n",
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--lemond", help="Path to an already-built lemond binary")
    parser.add_argument(
        "--check",
        action="store_true",
        help="Fail if docs or recorded examples are stale; run nothing",
    )
    parser.add_argument(
        "--bin-dir", help="Installed backends to reuse instead of downloading them"
    )
    args = parser.parse_args()

    binary = find_lemond(args.lemond)
    with Lemond(binary) as server:
        route_list = json.loads(server._get("/internal/routes", timeout=30))
    specs = {spec["id"]: spec for spec in route_list}
    validate_specs(specs)

    backend_versions = json.loads(BACKEND_VERSIONS.read_text(encoding="utf-8"))
    server_models = json.loads(SERVER_MODELS.read_text(encoding="utf-8"))
    cache = load_cache()

    keys = {}
    for spec in route_list:
        for example in spec["examples"]:
            keys[(spec["id"], example["name"])] = cache_key(
                specs, spec["id"], example, backend_versions, server_models
            )

    problems = []
    for spec in route_list:
        problems += lint_spec(spec)
    missing = [ref for ref, key in keys.items() if key not in cache]
    if missing and args.check:
        problems += [
            f"no recorded example for {route_id} {name!r}" for route_id, name in missing
        ]
    elif missing:
        print(f"Recording {len(missing)} example(s)...")
        with ExampleRunner(binary, args.bin_dir, specs) as runner:
            for route_id, name in missing:
                entry = runner.run(route_id, name)
                cache[keys[(route_id, name)]] = entry
                write_cache(cache)

    stale_keys = set(cache) - set(keys.values())
    if stale_keys:
        if args.check:
            problems.append(f"{len(stale_keys)} recorded example(s) no route uses")
        else:
            for key in stale_keys:
                del cache[key]
            write_cache(cache)

    rendered = {}
    for spec in route_list:
        entries = {}
        for example in spec["examples"]:
            entry = cache.get(keys[(spec["id"], example["name"])])
            if entry is None:
                continue
            check_entry(spec, find_response(spec, example["format"]), entry)
            entries[example["name"]] = entry
        if len(entries) == len(spec["examples"]):
            rendered[spec["id"]] = render_route(spec, entries)
            problems += lint_region(spec["id"], rendered[spec["id"]])

    pages: dict[str, list[dict]] = {}
    for spec in route_list:
        pages.setdefault(page_of(spec["id"]), []).append(spec)

    stale_pages = []
    for page, page_specs in pages.items():
        path = API_DOCS / f"{page}.md"
        original = (
            path.read_text(encoding="utf-8")
            if path.exists()
            else f"# {PAGE_TITLES.get(page, page)}\n"
        )
        current = {
            match.group(1): match.group(2) for match in REGION_RE.finditer(original)
        }
        rendered[f"{page}.summary"] = render_summary(
            page_specs, current.get(f"{page}.summary", ""), original
        )
        if any(spec["id"] not in rendered for spec in page_specs):
            continue
        updated, page_problems = update_page(
            original, page, page_specs, rendered, args.check
        )
        for problem in page_problems:
            if args.check:
                problems.append(f"{path.relative_to(REPO_ROOT)}: {problem}")
            else:
                print(f"{path.relative_to(REPO_ROOT)}: fixed: {problem}")
        if updated == original:
            continue
        if args.check:
            stale_pages.append(path)
            sys.stderr.write(
                "".join(
                    difflib.unified_diff(
                        original.splitlines(keepends=True),
                        updated.splitlines(keepends=True),
                        fromfile=str(path),
                        tofile=f"{path} (generated)",
                        n=1,
                    )
                )[:20000]
            )
        else:
            path.write_text(updated, encoding="utf-8", newline="\n")
            print(f"Updated {path.relative_to(REPO_ROOT)}")

    problems += [f"{path.relative_to(REPO_ROOT)} is stale" for path in stale_pages]
    if problems:
        for problem in problems:
            print(f"ERROR: {problem}", file=sys.stderr)
        if args.check:
            print(
                "\nAPI reference docs are stale. Run docs/tools/gen_api_boilerplate.py "
                "and commit the result.",
                file=sys.stderr,
            )
            return 1
    print("API reference docs are up to date." if args.check else "Done.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except GenerationError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)

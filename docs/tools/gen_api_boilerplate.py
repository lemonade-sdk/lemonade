#!/usr/bin/env python3
"""Generate the docs/api reference from lemond's route specs.

Every HTTP route declares its documentation in its RouteSpec (see
docs/dev/specs/http-server.md). This script follows gen_mcp_boilerplate.py: it
starts a lemond through the Lemond harness, reads every RouteSpec from
GET /internal/routes, and rewrites only the marker-delimited regions of the
docs/api pages:

    <!-- BEGIN GENERATED: <page>.summary -->   the page's summary table
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
    # Mirrors RequestMiddleware::authenticate(), which decides auth by path.
    if url.startswith("/internal/"):
        return (
            "Requires `LEMONADE_ADMIN_API_KEY` when it is set, otherwise "
            "`LEMONADE_API_KEY` when that is set."
        )
    if url.startswith(("/api/", "/v0/", "/v1/")) or url in ("/mcp", "/metrics"):
        return "Requires `LEMONADE_API_KEY` when it is set."
    return "Requires no API key."


def setup_chain(specs: dict, ref: dict, seen: tuple = ()) -> list[dict]:
    """Every example ref must run before ref, in order, each after its own setup."""
    response = find_response(specs, ref)
    chain = []
    for setup_ref in response["setup"]:
        key = (setup_ref["route"], setup_ref["format"])
        if key in seen:
            raise GenerationError(f"{ref['route']}: setup cycle through {key}")
        chain += setup_chain(specs, setup_ref, seen + (key,))
        chain.append(setup_ref)
    return chain


def find_response(specs: dict, ref: dict) -> dict:
    spec = specs.get(ref["route"])
    if spec is None:
        raise GenerationError(f"setup names unknown route {ref['route']}")
    for response in spec["responses"]:
        if response["format"] == ref["format"]:
            return response
    raise GenerationError(f"{ref['route']} has no {ref['format']} response")


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
    response: dict,
    backend_versions: dict,
    server_models: dict,
) -> str:
    ref = {"route": route_id, "format": response["format"]}
    setup = [
        {**setup_ref, "example": find_response(specs, setup_ref)["example"]}
        for setup_ref in setup_chain(specs, ref)
    ]
    examples = [response["example"]] + [entry["example"] for entry in reversed(setup)]
    payload = {
        "route": route_id,
        "format": response["format"],
        "example": response["example"],
        "setup": setup,
        "backend": pinned_backend(examples, backend_versions, server_models),
    }
    canonical = json.dumps(payload, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def validate_specs(specs: dict) -> None:
    for route_id, spec in specs.items():
        if route_id.endswith(".summary"):
            raise GenerationError(
                f"route id {route_id} collides with the summary region"
            )
        args = {arg["name"]: arg for arg in spec["args"]}
        for response in spec["responses"]:
            where = f"{route_id} {response['format']}"
            has_schema = bool(response["schema"])
            if response["format"] in JSON_FORMATS and not has_schema:
                raise GenerationError(f"{where}: a JSON format needs a schema")
            if response["format"] not in JSON_FORMATS and has_schema:
                raise GenerationError(f"{where}: only JSON formats have a schema")
            example = response["example"]
            if not isinstance(example, dict):
                raise GenerationError(
                    f"{where}: example must be an object of arguments"
                )
            for name in example:
                if name not in args:
                    raise GenerationError(
                        f"{where}: example names unknown argument {name}"
                    )
            chain_routes = {
                ref["route"]
                for ref in setup_chain(
                    specs, {"route": route_id, "format": response["format"]}
                )
            }
            for value in iter_strings(example):
                if value.startswith("$"):
                    target = value[1:].split("/", 1)[0]
                    if target not in chain_routes:
                        raise GenerationError(
                            f"{where}: {value} names a route that does not run in its setup"
                        )
                if (
                    value.startswith("@fixtures/")
                    and not (FIXTURES / value[10:]).exists()
                ):
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

    def run(self, route_id: str, response_format: str) -> dict:
        """Runs one example after its setup, returning its cache entry."""
        spec = self.specs[route_id]
        ref = {"route": route_id, "format": response_format}
        for setup_ref in setup_chain(self.specs, ref):
            self.run_one(setup_ref["route"], setup_ref["format"])
        return self.run_one(route_id, response_format)

    def run_one(self, route_id: str, response_format: str) -> dict:
        spec = self.specs[route_id]
        response = find_response(
            self.specs, {"route": route_id, "format": response_format}
        )
        self.ensure_running()
        request = self.build_request(spec, response["example"])
        print(f"  running {route_id} {response_format} ...", flush=True)
        recorded = self.send(request, response_format)
        if recorded["status"] >= 300:
            raise GenerationError(
                f"{route_id} {response_format} example answered {recorded['status']}: "
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


def render_event(event: str) -> str:
    rendered = []
    for line in event.splitlines():
        if line.startswith("data:"):
            payload = line[5:].strip()
            try:
                payload = json.dumps(json.loads(payload), ensure_ascii=False)
            except json.JSONDecodeError:
                pass
            rendered.append(f"data: {payload}")
        else:
            rendered.append(line)
    return "\n".join(rendered)


def render_recorded(fmt: str, entry: dict) -> str:
    recorded = entry["response"]
    status = f"`{recorded['status']}`"
    if fmt == "Json":
        return status + "\n\n" + fence("json", json_layout(recorded["body"]))
    if fmt in ("JsonLines", "EventStream"):
        render = (
            (lambda line: json.dumps(json.loads(line), ensure_ascii=False))
            if fmt == "JsonLines"
            else render_event
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


SCHEMA_KEY_ORDER = [
    "$schema",
    "$id",
    "title",
    "description",
    "type",
    "const",
    "enum",
    "required",
    "properties",
    "items",
    "additionalProperties",
    "oneOf",
    "anyOf",
    "allOf",
    "$defs",
]


def in_schema_order(schema):
    """lemond serves schemas with sorted keys; put each keyword where a reader expects it."""
    if isinstance(schema, list):
        return [in_schema_order(item) for item in schema]
    if not isinstance(schema, dict):
        return schema
    rank = {key: index for index, key in enumerate(SCHEMA_KEY_ORDER)}
    ordered = {}
    for key in sorted(schema, key=lambda k: rank.get(k, len(rank))):
        value = schema[key]
        if key in ("properties", "$defs") and isinstance(value, dict):
            value = {name: in_schema_order(sub) for name, sub in value.items()}
        elif key in ("items", "additionalProperties", "oneOf", "anyOf", "allOf"):
            value = in_schema_order(value)
        ordered[key] = value
    return ordered


def in_argument_order(spec: dict, entry: dict) -> dict:
    """lemond serves examples with sorted keys; show them in the route's argument order."""
    request = entry["request"]
    if not isinstance(request["json"], dict):
        return entry
    order = [arg["name"] for arg in spec["args"]]
    rank = {name: index for index, name in enumerate(order)}
    body = dict(
        sorted(request["json"].items(), key=lambda kv: rank.get(kv[0], len(order)))
    )
    return {**entry, "request": {**request, "json": body}}


def render_response(spec: dict, response: dict, entry: dict) -> str:
    entry = in_argument_order(spec, entry)
    fmt = response["format"]
    parts = [f"### Response: `{fmt}`\n"]
    parts.append(
        render_tab("PowerShell", fence("powershell", powershell_command(entry)))
    )
    parts.append(render_tab("Bash", fence("bash", bash_command(entry))))
    parts.append(render_tab("Response", render_recorded(fmt, entry)))
    if response["schema"]:
        parts.append(
            render_tab(
                "Schema",
                fence("json", json_layout(in_schema_order(response["schema"]))),
            )
        )
    return "\n".join(parts).rstrip()


def render_parameters(spec: dict) -> str:
    if not spec["args"]:
        return "### Parameters\n\nThis endpoint takes no parameters."
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
    urls = []
    for path in spec["paths"]:
        urls += urls_for(spec, path)
    others = [url for url in urls if url != primary_url(spec)]
    served = (
        f"Also served at {oxford([f'`{url}`' for url in others])}. " if others else ""
    )

    parts = [
        f"## {heading_of(spec)}",
        BADGE.format("status-" + route_status(spec)),
        "",
        spec["description"],
        "",
    ]
    for note in spec["notes"]:
        parts += [note, ""]
    parts += [served + auth_sentence(primary_url(spec))]
    if spec["responses"]:
        parts += ["", render_parameters(spec)]
    for response in spec["responses"]:
        parts += ["", render_response(spec, response, entries[response["format"]])]
    return "\n".join(parts)


def render_summary(page_specs: list[dict]) -> str:
    rows = [
        "| Method | Endpoint | Description |",
        "|--------|----------|-------------|",
    ]
    for spec in page_specs:
        heading = heading_of(spec)
        methods = ", ".join(f"`{method}`" for method in spec["methods"])
        rows.append(
            f"| {methods} | [`{primary_url(spec)}`](#{anchor(heading)}) | "
            f"{md_cell(spec['summary'])} |"
        )
    return "\n".join(rows)


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
        sorted(cache.items(), key=lambda kv: (kv[1]["route"], kv[1]["format"]))
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
        for response in spec["responses"]:
            keys[(spec["id"], response["format"])] = cache_key(
                specs, spec["id"], response, backend_versions, server_models
            )

    problems = []
    missing = [ref for ref, key in keys.items() if key not in cache]
    if missing and args.check:
        problems += [
            f"no recorded example for {route_id} {fmt}" for route_id, fmt in missing
        ]
    elif missing:
        print(f"Recording {len(missing)} example(s)...")
        with ExampleRunner(binary, args.bin_dir, specs) as runner:
            for route_id, fmt in missing:
                entry = runner.run(route_id, fmt)
                cache[keys[(route_id, fmt)]] = entry
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
        for response in spec["responses"]:
            entry = cache.get(keys[(spec["id"], response["format"])])
            if entry is None:
                continue
            check_entry(spec, response, entry)
            entries[response["format"]] = entry
        if len(entries) == len(spec["responses"]):
            rendered[spec["id"]] = render_route(spec, entries)

    pages: dict[str, list[dict]] = {}
    for spec in route_list:
        pages.setdefault(page_of(spec["id"]), []).append(spec)

    stale_pages = []
    for page, page_specs in pages.items():
        path = API_DOCS / f"{page}.md"
        rendered[f"{page}.summary"] = render_summary(page_specs)
        original = (
            path.read_text(encoding="utf-8")
            if path.exists()
            else f"# {PAGE_TITLES.get(page, page)}\n"
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

#!/usr/bin/env python3
"""Protect public HTTP/WebSocket routes by checking active C++ RouteSpecs.

This static, dependency-free guard checks RouteSpec declarations, their factory
registrations and the server's active route groups. Internal routes are excluded.
An intentional breaking change requires an explicit manifest edit in the PR.

Usage:
  python test/test_api_contract.py
  python test/test_api_contract.py --dump-implemented
"""

# API Contract checker: RouteSpec-backed
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import NamedTuple

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "test/api_contract_manifest.json"
SOURCES = ROOT / "src/cpp/server"
ROUTES_DIR = SOURCES / "routes"
SPEC_RE = re.compile(r"\bRouteSpec\s+spec\s*\(\s*\)\s*const\s+override\s*\{")
FACTORY_RE = re.compile(r"\bstd::unique_ptr\s*<\s*ApiRoute\s*>\s*(make_\w+_route)\s*\(")
GROUP_RE = re.compile(r"\bvoid\s+(register_\w+_routes)\s*\(")
ADD_RE = re.compile(r"\bregistry\s*\.\s*add\s*\(\s*(make_\w+_route)\s*\(")
LITERAL_RE = re.compile(r'"(?:\\.|[^"\\])*"')
ALL_METHODS = {"GET", "POST", "PUT", "DELETE"}


class Route(NamedTuple):
    method: str
    path: str


def fail(message: str) -> None:
    raise ValueError(message)


def string_end(code: str, index: int) -> int:
    """Skip quoted characters/strings, including delimited C++ raw literals."""
    if code.startswith('R"', index):
        match = re.match(r'R"([^ ()\\\t\r\n]{0,16})\(', code[index:])
        if match:
            delimiter = match.group(1)
            end = code.find(")" + delimiter + '"', index + match.end())
            return len(code) if end < 0 else end + len(delimiter) + 2
    # C++ digit separators (e.g. 1'000) are not character literals.
    if code[index] == "'" and 0 < index < len(code) - 1:
        if code[index - 1].isalnum() and code[index + 1].isalnum():
            return index + 1
    quote = code[index]
    if quote not in {'"', "'"}:
        return index
    index += 1
    while index < len(code):
        if code[index] == "\\":
            index += 2
        elif code[index] == quote:
            return index + 1
        else:
            index += 1
    return index


def strip_comments(code: str) -> str:
    # Preserve offsets and newlines for diagnostics and brace matching.
    out = list(code)
    i = 0
    while i < len(code):
        if code.startswith('R"', i) or code[i] in {'"', "'"}:
            next_i = string_end(code, i)
            if next_i > i:
                i = next_i
                continue
        if code.startswith("//", i):
            j = code.find("\n", i)
            j = len(code) if j < 0 else j
            for n in range(i, j):
                out[n] = " "
            i = j
        elif code.startswith("/*", i):
            j = code.find("*/", i + 2)
            j = len(code) if j < 0 else j + 2
            for n in range(i, j):
                if out[n] != "\n":
                    out[n] = " "
            i = j
        else:
            i += 1
    return "".join(out)


def active_registrations(code: str, path: Path) -> str:
    """Never treat conditionally compiled route registrations as active."""
    if re.search(r"(?m)^\s*#\s*(?:if|ifdef|ifndef|elif|else|endif)\b", code):
        fail(
            f"{path}: conditional registration needs explicit review; no C++ preprocessor is run"
        )
    return code


def brace_body(code: str, open_brace: int) -> str:
    depth = 0
    i = open_brace
    while i < len(code):
        if code.startswith('R"', i) or code[i] in {'"', "'"}:
            next_i = string_end(code, i)
            if next_i > i:
                i = next_i
                continue
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return code[open_brace + 1 : i]
        i += 1
    fail("unclosed C++ brace (possibly unsupported string literal)")


def string_list(body: str, field: str, path: Path) -> list[str]:
    # A later assignment overrides the first one in C++; never protect stale paths.
    if len(re.findall(rf"\bs\s*\.\s*{field}\s*=", body)) != 1:
        fail(f"{path}: expected exactly one assignment to s.{field}")
    match = re.search(rf"\bs\s*\.\s*{field}\s*=\s*(\{{[^;]*\}})\s*;", body, re.DOTALL)
    if not match:
        fail(f"{path}: cannot statically read RouteSpec s.{field}")
    raw = match.group(1)
    parts: list[str] = []
    current = ""
    previous = 1  # after the initializer's opening brace
    # C++ joins adjacent string literals, but commas start a new path/method.
    for literal in LITERAL_RE.finditer(raw):
        between = raw[previous : literal.start()]
        if re.sub(r"[,\s]", "", between) or between.count(",") > 1:
            fail(f"{path}: nonliteral RouteSpec s.{field}: {raw[:120]}")
        if "," in between:
            if not current:
                fail(f"{path}: empty RouteSpec s.{field} item")
            parts.append(current)
            current = ""
        current += json.loads(literal.group())
        previous = literal.end()
    if not current or raw[previous:-1].strip() not in {"", ","}:
        fail(f"{path}: nonliteral RouteSpec s.{field}: {raw[:120]}")
    parts.append(current)
    return parts


def normalize(path: str) -> str:
    path = re.sub(r"\{[^{}]+\}", "{param}", path)
    return path


def ws_path(path: str) -> str:
    """WebSocket paths are exact strings: a trailing slash is significant."""
    return path if path.startswith("/") else "/" + path


def parse_route_spec(code: str, path: Path) -> list[tuple[str, list[str], list[str]]]:
    code = strip_comments(code)
    results = []
    for match in SPEC_RE.finditer(code):
        body = brace_body(code, match.end() - 1)
        if not re.search(r"\breturn\s+s\s*;\s*$", body):
            fail(f"{path}: unsupported RouteSpec return; expected return s;")
        methods = string_list(body, "methods", path)
        paths = string_list(body, "paths", path)
        unknown = set(methods) - ALL_METHODS
        if unknown:
            fail(f"{path}: unsupported HTTP methods: {sorted(unknown)}")
        if len(re.findall(r"\bs\s*\.\s*prefixes\s*=", body)) > 1:
            fail(f"{path}: multiple s.prefixes assignments")
        prefix = re.search(r"\bs\s*\.\s*prefixes\s*=\s*Prefixes::(\w+)\s*;", body)
        if re.search(r"\bs\s*\.\s*prefixes\s*=", body) and not prefix:
            fail(f"{path}: cannot parse s.prefixes")
        kind = prefix.group(1) if prefix else "Quad"
        if kind not in {"Quad", "Root", "Internal"}:
            fail(f"{path}: unknown prefix family {kind}")
        results.append((kind, methods, paths))
    return results


def parse_quad_prefixes(code: str) -> list[str]:
    match = re.search(
        r"\bkQuadPrefixes\s*\[\s*\]\s*=\s*\{([^}]+)\}", strip_comments(code), re.DOTALL
    )
    if not match:
        fail("RouteRegistry quad prefix definition changed; update this checker")
    return json_strings(match.group(1))


def json_strings(source: str) -> list[str]:
    strings = LITERAL_RE.findall(source)
    # Commas, not adjacent quoted tokens, delimit C++ initializer elements.
    elements = [p for p in LITERAL_RE.sub('""', source).split(",") if p.strip()]
    if len(elements) != len(strings):
        fail("Adjacent C++ string literals in prefix initializer")
    if re.sub(r"[{},\s]", "", LITERAL_RE.sub("", source)):
        fail(f"unsupported literal list: {source[:100]!r}")
    return [json.loads(value) for value in strings]


def active_http_routes(manifest: dict) -> set[Route]:
    for source in manifest["sources"]:
        if not (ROOT / source).exists():
            fail(f"manifest source missing: {source}")

    spec_header = strip_comments(
        (ROOT / "src/cpp/include/lemon/server/api_route.h").read_text(encoding="utf-8")
    )
    spec_start = spec_header.find("struct RouteSpec")
    if spec_start < 0 or not re.search(
        r"\bPrefixes\s+prefixes\s*=\s*Prefixes::Quad\s*;",
        brace_body(spec_header, spec_header.find("{", spec_start)),
    ):
        fail("RouteSpec default prefix is not Quad; review API contract")

    quad = parse_quad_prefixes(
        (SOURCES / "core/route_registry.cpp").read_text(encoding="utf-8")
    )
    if quad != [prefix + "/" for prefix in manifest["versioned_prefixes"]]:
        fail(
            f"Quad prefixes changed: code={quad}, manifest={manifest['versioned_prefixes']}"
        )

    server = strip_comments((SOURCES / "server.cpp").read_text(encoding="utf-8"))
    ctor = re.search(r"\bServer::Server\s*\(", server)
    if not ctor:
        fail("Server constructor not found")
    ctor_body = brace_body(server, server.find("{", ctor.end()))
    begin = ctor_body.find("registry_ = std::make_unique<RouteRegistry>();")
    end = ctor_body.find("middleware_ = std::make_unique<RequestMiddleware>", begin)
    if begin < 0 or end < 0:
        fail("Server route registration block changed; review API contract")
    block = ctor_body[begin:end]
    active_groups: set[str] = set()
    for line in block.splitlines():
        line = line.strip()
        if not line or line in {
            "registry_ = std::make_unique<RouteRegistry>();",
            "ctx_.registry = registry_.get();",
        }:
            continue
        call = re.fullmatch(
            r"(register_\w+_routes)\s*\(\s*\*registry_\s*,\s*ctx_\s*\)\s*;",
            line,
        )
        if not call or call.group(1) in active_groups:
            fail("Unsupported or duplicate Server route registration: " + line)
        active_groups.add(call.group(1))
    if not active_groups:
        fail("Could not find active route group registrations in Server constructor")

    registered: set[str] = set()
    group_definitions: set[str] = set()
    for path in ROUTES_DIR.rglob("routes.cpp"):
        source = active_registrations(
            strip_comments(path.read_text(encoding="utf-8")), path
        )
        names = set(GROUP_RE.findall(source))
        group_definitions.update(names)
        if not names.intersection(active_groups):
            continue
        if len(names) != 1:
            fail(f"{path}: expected one route registration group, found {names}")
        group = GROUP_RE.search(source)
        group_body = brace_body(source, source.find("{", group.end()))
        if re.search(
            r"\b(?:if|for|while|switch|do|return|goto|try|throw)\b", group_body
        ):
            fail(f"{path}: conditional or early-exit route registration unsupported")
        factories = ADD_RE.findall(group_body)
        if len(factories) != len(re.findall(r"\bregistry\s*\.\s*add\s*\(", group_body)):
            fail(f"{path}: unsupported or missing registry.add() registration")
        if len(factories) != len(set(factories)) or set(factories).intersection(
            registered
        ):
            fail(f"{path}: duplicate route factory registration")
        registered.update(factories)

    unrecognized = active_groups - group_definitions
    if unrecognized:
        fail(f"Active route groups without routes.cpp: {sorted(unrecognized)}")

    definitions: dict[str, tuple[str, list[str], list[str]]] = {}
    for path in ROUTES_DIR.rglob("*.cpp"):
        if path.name == "routes.cpp":
            continue
        code = path.read_text(encoding="utf-8")
        factories = FACTORY_RE.findall(strip_comments(code))
        if not registered.intersection(factories):
            continue
        specs = parse_route_spec(code, path)
        if len(specs) != 1 or len(factories) != 1:
            fail(
                f"{path}: expected one RouteSpec and one factory, found {len(specs)}, {factories}"
            )
        if factories[0] in definitions:
            fail(f"{path}: duplicate factory {factories[0]}")
        definitions[factories[0]] = specs[0]

    unknown_factories = registered - definitions.keys()
    if unknown_factories:
        fail(
            f"Registered factories without a parsed RouteSpec: {sorted(unknown_factories)}"
        )

    routes: set[Route] = set()
    for factory in registered:
        kind, methods, paths = definitions[factory]
        if kind == "Internal":
            continue  # Internal implementation endpoints are not public API.
        prefixes = quad if kind == "Quad" else [""]
        for method in methods:
            for path in paths:
                for prefix in prefixes:
                    routes.add(
                        Route(
                            method, normalize(prefix + path if kind == "Quad" else path)
                        )
                    )
    return routes


def active_websocket_routes(manifest: dict) -> set[Route]:
    code = strip_comments(
        (SOURCES / "websocket_server.cpp").read_text(encoding="utf-8")
    )
    marker = code.find("WebSocketServer::classify_path")
    if marker < 0:
        fail("WebSocketServer::classify_path not found")
    body = brace_body(code, code.find("{", marker))
    prefix_match = re.search(r"for\s*\([^:]+:\s*\{([^}]+)\}\)", body, re.DOTALL)
    if not prefix_match:
        fail("Could not derive WebSocket version prefixes")
    # Validate the current, narrow prefix-stripping idiom before deriving aliases.
    loop_body = brace_body(body, body.find("{", prefix_match.end()))
    required = (
        (body, r"\bstd::string\s+stripped\s*=\s*path\s*;"),
        (loop_body, r"\bstripped\.rfind\s*\(\s*prefix\s*,\s*0\s*\)\s*==\s*0"),
        (loop_body, r"\bstripped\.size\s*\(\s*\)\s*>\s*len"),
        (loop_body, r"\bstripped\s*\[\s*len\s*\]\s*==\s*'/'"),
        (
            loop_body,
            r"\bstripped\s*=\s*stripped\.substr\s*\(\s*len\s*\)\s*;\s*break\s*;",
        ),
    )
    if any(not re.search(pattern, text) for text, pattern in required):
        fail("WebSocket prefix stripping changed; inspect classify_path")
    if re.search(r'\bpath\s*==\s*"', body):
        fail("WebSocket raw-path comparison unsupported; inspect classify_path")
    prefixes = json_strings(prefix_match.group(1))
    if prefixes != manifest["versioned_prefixes"]:
        fail(
            f"WebSocket prefixes changed: code={prefixes}; manifest={manifest['versioned_prefixes']}"
        )
    comparison = r'stripped\s*==\s*"[^"]+"'
    accepted = re.compile(
        rf"\bif\s*\(\s*(?P<conditions>{comparison}(?:\s*\|\|\s*{comparison})*)\s*\)"
        r"\s*\{\s*return\s+ConnectionKind::(?P<kind>\w+)\s*;\s*\}",
        re.DOTALL,
    )
    branches = list(accepted.finditer(body))
    if (
        len(re.findall(r"\bif\s*\(", body))
        != len(branches) + len(re.findall(r"\bif\s*\(", loop_body))
        or len(re.findall(r"\breturn\s+ConnectionKind::\w+\s*;", body))
        != len(branches) + 1
        or not re.search(r"\breturn\s+ConnectionKind::invalid\s*;\s*$", body)
    ):
        fail("Unsupported WebSocket classifier control flow; review routes")
    paths: set[str] = set()
    for branch in branches:
        if branch.group("kind") == "invalid":
            continue
        paths.update(
            re.findall(r'\bstripped\s*==\s*"([^"]+)"', branch.group("conditions"))
        )
    if not paths:
        fail("Could not derive accepted WebSocket paths from classify_path")

    routes: set[Route] = set()
    for path in paths:
        for prefix in [""] + prefixes:
            routes.add(Route("WS", ws_path(prefix + path)))
    return routes


def expanded_manifest(manifest: dict, name: str) -> set[Route]:
    routes = set()
    canonical = ws_path if name == "websocket_routes" else normalize
    for item in manifest[name]:
        if "path" in item and "versioned_path" in item:
            fail(f"Ambiguous manifest route: {item}")
        if "path" in item:
            routes.add(Route(item["method"], canonical(item["path"])))
        elif "versioned_path" in item:
            for prefix in manifest["versioned_prefixes"]:
                routes.add(
                    Route(
                        item["method"], canonical(f"{prefix}/{item['versioned_path']}")
                    )
                )
        else:
            fail(f"Malformed manifest route: {item}")
    return routes


def show(routes: set[Route]) -> str:
    return "\n".join(f"  {m:7} {path}" for m, path in sorted(routes))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dump-implemented", action="store_true")
    args = parser.parse_args()
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    http = active_http_routes(manifest)
    ws = active_websocket_routes(manifest)
    if args.dump_implemented:
        print(show(http | ws))
        return 0
    declared_http = expanded_manifest(manifest, "http_routes")
    declared_ws = expanded_manifest(manifest, "websocket_routes")
    missing = (declared_http - http) | (declared_ws - ws)
    unlisted = (http - declared_http) | (ws - declared_ws)
    if missing:
        print("API contract check FAILED: previously protected routes are missing:")
        print(show(missing))
        print(
            "Review each route against main; do not silently remove protected entries."
        )
    if unlisted:
        print(
            "API contract check FAILED: newly registered public routes are not protected:"
        )
        print(show(unlisted))
        print(
            "Add the new public routes to test/api_contract_manifest.json in the same PR."
        )
    if missing or unlisted:
        return 1
    print(
        f"Public API contract check passed ({len(declared_http | declared_ws)} protected routes)."
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, FileNotFoundError, json.JSONDecodeError) as error:
        print(f"API contract parser ERROR: {error}", file=sys.stderr)
        sys.exit(2)

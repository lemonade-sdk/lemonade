#!/usr/bin/env python3
"""Verify Lemonade's public API route contract against C++ route registrations.

This is intentionally static and dependency-free. It catches accidental breaking
changes early in CI without building the server or downloading models. When a
public API change is intentional, update test/api_contract_manifest.json in the
same PR so reviewers get a clear contract diff.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Iterable, NamedTuple


REPO_ROOT = Path(__file__).resolve().parents[1]
MANIFEST_PATH = REPO_ROOT / "test" / "api_contract_manifest.json"


class Route(NamedTuple):
    method: str
    path: str


ROUTE_CALL_RE = re.compile(
    r"\b(?:web_server|server)\."
    r"(?P<method>Get|Post|Delete|Put|Patch)\(\s*"
    r"(?P<literal>R\"\(.*?\)\"|\"[^\"]+\")"
    r"(?P<concat>\s*\+\s*endpoint)?",
    re.DOTALL,
)

REGISTER_CALL_RE = re.compile(
    r"\b(?P<helper>register_get|register_post)\(\s*\"(?P<endpoint>[^\"]+)\""
)

HELPER_METHODS = {
    "register_get": "GET",
    "register_post": "POST",
}

CPP_TO_HTTP_METHOD = {
    "Get": "GET",
    "Post": "POST",
    "Delete": "DELETE",
    "Put": "PUT",
    "Patch": "PATCH",
}


def strip_cpp_comments(source: str) -> str:
    """Remove C/C++ comments while preserving strings and line positions."""

    out: list[str] = []
    i = 0
    n = len(source)

    while i < n:
        # Preserve raw strings used for httplib regex routes, e.g. R"(/v1/foo/(.+))".
        if source.startswith('R"(', i):
            end = source.find(')"', i + 3)
            if end == -1:
                out.append(source[i:])
                break
            out.append(source[i : end + 2])
            i = end + 2
            continue

        char = source[i]
        nxt = source[i + 1] if i + 1 < n else ""

        if char == '"':
            out.append(char)
            i += 1
            while i < n:
                out.append(source[i])
                if source[i] == "\\" and i + 1 < n:
                    i += 1
                    out.append(source[i])
                elif source[i] == '"':
                    i += 1
                    break
                i += 1
            continue

        if char == "'":
            out.append(char)
            i += 1
            while i < n:
                out.append(source[i])
                if source[i] == "\\" and i + 1 < n:
                    i += 1
                    out.append(source[i])
                elif source[i] == "'":
                    i += 1
                    break
                i += 1
            continue

        if char == "/" and nxt == "/":
            i += 2
            while i < n and source[i] != "\n":
                i += 1
            if i < n:
                out.append("\n")
                i += 1
            continue

        if char == "/" and nxt == "*":
            i += 2
            while i + 1 < n and not (source[i] == "*" and source[i + 1] == "/"):
                if source[i] == "\n":
                    out.append("\n")
                i += 1
            i += 2 if i + 1 < n else 0
            continue

        out.append(char)
        i += 1

    return "".join(out)


def load_manifest() -> dict:
    with MANIFEST_PATH.open(encoding="utf-8") as f:
        return json.load(f)


def normalize_path(path: str) -> str:
    path = path.strip()
    path = re.sub(r"\(\.\+\)", "{param}", path)
    path = re.sub(r"\(\\d\+\)", "{param}", path)
    path = path.replace("//", "/")
    if not path.startswith("/"):
        path = "/" + path
    return path.rstrip("/") or "/"


def decode_cpp_literal(literal: str) -> str:
    if literal.startswith('R"(') and literal.endswith(')"'):
        return literal[3:-2]
    if literal.startswith('"') and literal.endswith('"'):
        return literal[1:-1]
    raise ValueError(f"unsupported C++ string literal: {literal!r}")


def find_helper_body(source: str, helper_name: str) -> str:
    start = source.find(f"auto {helper_name}")
    if start == -1:
        return ""

    brace = source.find("{", start)
    if brace == -1:
        return ""

    depth = 0
    for index in range(brace, len(source)):
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[brace : index + 1]
    return ""


def extract_helper_prefixes(source: str, helper_name: str, cpp_method: str) -> list[str]:
    body = find_helper_body(source, helper_name)
    if not body:
        return []

    pattern = re.compile(
        rf"\bweb_server\.{cpp_method}\(\s*\"(?P<prefix>[^\"]*)\"\s*\+\s*endpoint\s*,\s*handler"
    )
    prefixes = []
    for match in pattern.finditer(body):
        prefixes.append(normalize_path(match.group("prefix")))
    return prefixes


def extract_routes_from_source(source: str) -> set[Route]:
    source = strip_cpp_comments(source)
    routes: set[Route] = set()

    get_prefixes = extract_helper_prefixes(source, "register_get", "Get")
    post_prefixes = extract_helper_prefixes(source, "register_post", "Post")

    for match in REGISTER_CALL_RE.finditer(source):
        helper = match.group("helper")
        endpoint = match.group("endpoint")
        method = HELPER_METHODS[helper]
        prefixes = get_prefixes if helper == "register_get" else post_prefixes
        for prefix in prefixes:
            routes.add(Route(method, normalize_path(f"{prefix}/{endpoint}")))

    for match in ROUTE_CALL_RE.finditer(source):
        if match.group("concat"):
            # These are helper definitions such as web_server.Get("/v1/" + endpoint, handler).
            # The expanded helper calls above are the contract-relevant routes.
            continue
        method = CPP_TO_HTTP_METHOD[match.group("method")]
        path = normalize_path(decode_cpp_literal(match.group("literal")))
        routes.add(Route(method, path))

    return routes


def extract_websocket_routes(source: str) -> set[Route]:
    source = strip_cpp_comments(source)
    return {
        Route("WS", normalize_path(path))
        for path in re.findall(r"path\s*==\s*\"([^\"]+)\"", source)
    }


def expected_routes(manifest: dict) -> set[Route]:
    prefixes = manifest["versioned_prefixes"]
    routes: set[Route] = set()

    for route in manifest["http_routes"]:
        method = route["method"]
        if "path" in route:
            routes.add(Route(method, normalize_path(route["path"])))
            continue
        suffix = route["versioned_path"].lstrip("/")
        for prefix in prefixes:
            routes.add(Route(method, normalize_path(f"{prefix}/{suffix}")))

    for route in manifest["websocket_routes"]:
        routes.add(Route(route["method"], normalize_path(route["path"])))

    return routes


def implemented_routes(manifest: dict) -> set[Route]:
    routes: set[Route] = set()
    missing_sources: list[Path] = []

    for relative_source in manifest["sources"]:
        source_path = REPO_ROOT / relative_source
        if not source_path.exists():
            missing_sources.append(source_path)
            continue

        source = source_path.read_text(encoding="utf-8")
        if relative_source.endswith("websocket_server.cpp"):
            routes.update(extract_websocket_routes(source))
        else:
            routes.update(extract_routes_from_source(source))

    if missing_sources:
        paths = "\n".join(f"  - {path.relative_to(REPO_ROOT)}" for path in missing_sources)
        raise FileNotFoundError(f"API contract source file(s) missing:\n{paths}")

    return routes


def format_routes(routes: Iterable[Route]) -> str:
    return "\n".join(f"  - {route.method:6} {route.path}" for route in sorted(routes))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--dump-implemented",
        action="store_true",
        help="Print the route registrations discovered in the C++ sources.",
    )
    args = parser.parse_args()

    manifest = load_manifest()
    expected = expected_routes(manifest)
    actual = implemented_routes(manifest)

    if args.dump_implemented:
        print(format_routes(actual))
        return 0

    missing = expected - actual
    if missing:
        print("Public API contract check failed: expected route(s) are missing from C++ route registrations.")
        print()
        print(format_routes(missing))
        print()
        print(
            "If this API change is intentional, update test/api_contract_manifest.json "
            "in the same PR so reviewers can evaluate the contract change."
        )
        return 1

    print(f"Public API contract check passed ({len(expected)} expected routes).")
    return 0


if __name__ == "__main__":
    sys.exit(main())

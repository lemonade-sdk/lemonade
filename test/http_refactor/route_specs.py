"""Data-only checks for the server's GET /internal/routes manifest."""

from __future__ import annotations

import re
from pathlib import Path
from typing import Any

QUAD = ("/api/v0/", "/api/v1/", "/v0/", "/v1/")
METHODS = {"GET", "POST", "PUT", "DELETE"}
FORMATS = {"Raw", "Json", "OptionalJson", "Form"}
RESPONSES = {
    "Json",
    "JsonLines",
    "EventStream",
    "Text",
    "Binary",
    "BinaryStream",
    "Empty",
}
LOCATIONS = {"JsonBody", "Query", "Path", "Form"}
TYPES = {"string", "integer", "number", "boolean", "array", "object", "null"}
PARAM = re.compile(r"\{([^{}]+)\}")


def expanded_paths(route: dict[str, Any]) -> list[str]:
    prefix = route["prefixes"]
    paths = route["paths"]
    if prefix == "Quad":
        return [base + path for path in paths for base in QUAD]
    if prefix == "Internal":
        return ["/internal/" + path for path in paths]
    if prefix == "Root":
        return list(paths)
    raise ValueError(f"Unknown prefix {prefix!r}")


def sample_path(route: dict[str, Any], path: str) -> str:
    arguments = {arg["name"]: arg for arg in route["args"] if arg["in"] == "Path"}

    def replace(match: re.Match[str]) -> str:
        schema = arguments[match.group(1)]["schema"]
        return "1" if schema.get("type") == "integer" else "test"

    return PARAM.sub(replace, path)


def validate_schema(schema: Any, address: str, faults: list[str]) -> None:
    if not isinstance(schema, dict):
        faults.append(f"{address}: schema must be an object")
        return
    if "type" in schema:
        types = schema["type"]
        if isinstance(types, str):
            types = [types]
        if (
            not isinstance(types, list)
            or not types
            or any(t not in TYPES for t in types)
        ):
            faults.append(f"{address}: invalid JSON schema type {types!r}")
    if "required" in schema:
        props = schema.get("properties")
        req = schema["required"]
        if not isinstance(req, list) or any(not isinstance(k, str) for k in req):
            faults.append(f"{address}: required must be a list of strings")
        elif isinstance(props, dict):
            for key in req:
                if key not in props:
                    faults.append(f"{address}: required key {key} has no schema")
    if "properties" in schema:
        if not isinstance(schema["properties"], dict):
            faults.append(f"{address}: properties must be an object")
        else:
            for key, entry in schema["properties"].items():
                validate_schema(entry, f"{address}.properties.{key}", faults)
    if "items" in schema:
        validate_schema(schema["items"], address + ".items", faults)
    if isinstance(schema.get("additionalProperties"), dict):
        validate_schema(
            schema["additionalProperties"], address + ".additionalProperties", faults
        )


def validate_routes(routes: Any) -> list[str]:
    faults = []
    if not isinstance(routes, list) or not routes:
        return ["route manifest must be a nonempty array"]
    ids = set()
    method_paths: dict[tuple[str, str], str] = {}
    for route in routes:
        if not isinstance(route, dict):
            faults.append(f"invalid route: {route!r}")
            continue
        key = route.get("id", "<missing-id>")
        if not isinstance(key, str) or not re.fullmatch(r"[a-z0-9_]+\.[a-z0-9_]+", key):
            faults.append(f"malformed route id: {key!r}")
            continue
        if key in ids:
            faults.append(f"duplicate route id: {key}")
        ids.add(key)
        methods, paths = route.get("methods"), route.get("paths")
        if (
            not isinstance(methods, list)
            or not methods
            or any(m not in METHODS for m in methods)
        ):
            faults.append(f"{key}: missing/invalid methods")
            continue
        if (
            not isinstance(paths, list)
            or not paths
            or not all(isinstance(p, str) for p in paths)
        ):
            faults.append(f"{key}: missing/invalid paths")
            continue
        if route.get("request_format") not in FORMATS:
            faults.append(f"{key}: invalid request format")
        if route.get("error_format") not in ("Plain", "OpenAi", "Anthropic"):
            faults.append(f"{key}: invalid error format")
        if route.get("prefixes") not in ("Quad", "Internal", "Root"):
            faults.append(f"{key}: invalid prefix")
            continue
        if route["prefixes"] == "Root" and any(not p.startswith("/") for p in paths):
            faults.append(f"{key}: root URL not absolute")
        if route["prefixes"] != "Root" and any(p.startswith("/") for p in paths):
            faults.append(f"{key}: prefixed path starts with slash")
        args = route.get("args", [])
        if not isinstance(args, list):
            faults.append(f"{key}: args must be an array")
            continue
        names = set()
        path_arg_names = {arg.get("name") for arg in args if arg.get("in") == "Path"}
        for arg in args:
            name = arg.get("name")
            if not isinstance(name, str) or not name:
                faults.append(f"{key}: unnamed argument")
                continue
            if (name, arg.get("in")) in names:
                faults.append(f"{key}: duplicate arg {name}")
            names.add((name, arg.get("in")))
            if arg.get("in") not in LOCATIONS or not isinstance(
                arg.get("required"), bool
            ):
                faults.append(f"{key}.{name}: invalid location/required")
            if not isinstance(arg.get("aliases"), list):
                faults.append(f"{key}.{name}: aliases must be an array")
            if not isinstance(arg.get("forwarded"), bool):
                faults.append(f"{key}.{name}: forwarded must be boolean")
            validate_schema(arg.get("schema"), f"{key}.{name}", faults)
        params = list(PARAM.findall(paths[0]))
        if len(set(params)) != len(params) or set(params) != path_arg_names:
            faults.append(f"{key}: path parameters disagree with path arguments")
        if any(PARAM.findall(path) != params for path in paths):
            faults.append(f"{key}: aliases change positional path arguments")
        responses = route.get("responses", [])
        formats = (
            [r.get("format") for r in responses] if isinstance(responses, list) else []
        )
        if any(fmt not in RESPONSES for fmt in formats):
            faults.append(f"{key}: invalid response formats")
        if not formats and not route.get("errors"):
            faults.append(f"{key}: no response or documented error")
        for response in responses if isinstance(responses, list) else []:
            if response.get("format") in (
                "Json",
                "JsonLines",
                "EventStream",
            ) and isinstance(response.get("schema"), dict):
                validate_schema(
                    response["schema"], f"{key}.response.{response['format']}", faults
                )
        for path in expanded_paths(route):
            for method in methods:
                identity = method, path
                if identity in method_paths:
                    faults.append(
                        f"{key}: {method} {path} duplicates {method_paths[identity]}"
                    )
                else:
                    method_paths[identity] = key
    for route in routes:
        for example in route.get("examples", []):
            if example.get("format") not in [
                r.get("format") for r in route.get("responses", [])
            ]:
                faults.append(
                    f"{route['id']}: example declares an unknown response format"
                )
            for setup in example.get("setup", []):
                if setup.get("route") not in ids:
                    faults.append(
                        f"{route['id']}: example setup targets missing {setup.get('route')}"
                    )
    return faults


def validate_docs(routes: list[dict[str, Any]], root: Path) -> list[str]:
    faults = []
    pages: dict[str, str] = {}
    for route in routes:
        key = route["id"]
        page = key.split(".", 1)[0]
        if page not in pages:
            filename = root / "docs" / "api" / f"{page}.md"
            if not filename.is_file():
                faults.append(f"missing generated docs page {filename}")
                continue
            pages[page] = filename.read_text(encoding="utf-8")
        if f"<!-- BEGIN GENERATED: {key} -->" not in pages[page]:
            faults.append(f"docs/api/{page}.md lacks generated section {key}")
    return faults


def check_json(schema: dict[str, Any], value: Any, path: str = "$") -> list[str]:
    faults: list[str] = []
    types = schema.get("type", [])
    if isinstance(types, str):
        types = [types]
    predicates = {
        "string": lambda v: isinstance(v, str),
        "integer": lambda v: isinstance(v, int) and not isinstance(v, bool),
        "number": lambda v: isinstance(v, (int, float)) and not isinstance(v, bool),
        "boolean": lambda v: isinstance(v, bool),
        "array": lambda v: isinstance(v, list),
        "object": lambda v: isinstance(v, dict),
        "null": lambda v: v is None,
    }
    if types and not any(predicates[t](value) for t in types):
        return [f"{path}: expected {types!r}, got {type(value).__name__}"]
    if "const" in schema and value != schema["const"]:
        faults.append(f"{path}: unexpected const value {value!r}")
    if "enum" in schema and value not in schema["enum"]:
        faults.append(f"{path}: {value!r} not in enum")
    if isinstance(value, dict):
        for key in schema.get("required", []):
            if key not in value:
                faults.append(f"{path}: missing {key}")
        props = schema.get("properties", {})
        for key, item in value.items():
            item_schema = props.get(key, schema.get("additionalProperties"))
            if isinstance(item_schema, dict):
                faults.extend(check_json(item_schema, item, f"{path}.{key}"))
    elif isinstance(value, list) and isinstance(schema.get("items"), dict):
        for index, item in enumerate(value):
            faults.extend(check_json(schema["items"], item, f"{path}[{index}]"))
    return faults

#!/usr/bin/env python3
"""Generate MCP tool reference boilerplate from a live Lemonade server.

This deliberately follows docs/tools/gen_backend_boilerplate.py:

* lemond is built outside this generator;
* find_lemond() and the throwaway Lemond process lifecycle are reused;
* tool metadata is read from the live /mcp tools/list response;
* only the marker-delimited generated region is rewritten;
* --check fails when the committed generated reference drifts.
"""

from __future__ import annotations

import argparse
import difflib
import json
import os
import sys
import urllib.request
from pathlib import Path

from gen_backend_boilerplate import Lemond, find_lemond

REPO_ROOT = Path(__file__).resolve().parents[2]
TARGET_DOC = REPO_ROOT / "docs" / "api" / "mcp.md"
BEGIN = "<!-- BEGIN GENERATED: mcp-tools -->"
END = "<!-- END GENERATED: mcp-tools -->"


def auth_headers() -> dict[str, str]:
    key = os.environ.get("LEMONADE_ADMIN_API_KEY") or os.environ.get("LEMONADE_API_KEY")
    return {"Authorization": f"Bearer {key}"} if key else {}


def tools_list(port: int) -> list[dict]:
    payload = {
        "jsonrpc": "2.0",
        "id": "docs",
        "method": "tools/list",
        "params": {},
    }
    request = urllib.request.Request(
        f"http://127.0.0.1:{port}/mcp",
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json", **auth_headers()},
        method="POST",
    )
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(request, timeout=10) as response:
        body = json.loads(response.read())
    if "error" in body:
        raise RuntimeError(f"tools/list failed: {json.dumps(body['error'])}")
    tools = body.get("result", {}).get("tools")
    if not isinstance(tools, list):
        raise RuntimeError("tools/list response did not contain result.tools[]")
    return tools


def md_text(text: object) -> str:
    """Render prose without letting raw <...> disappear as HTML.

    Backtick-delimited inline-code spans are left alone; outside them, HTML
    metacharacters are escaped. This keeps values such as image_<token>.png
    visible while preserving intentionally formatted `code` spans.
    """
    parts = str(text).replace("\n", " ").split("`")
    for index in range(0, len(parts), 2):
        parts[index] = (
            parts[index].replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        )
    return "`".join(parts)


def md_cell(text: object) -> str:
    return md_text(text).replace("|", "\\|")


def compact_schema(schema: dict) -> str:
    body = {key: value for key, value in schema.items() if key != "description"}
    return json.dumps(body, ensure_ascii=False, separators=(",", ":"))


def render_tool(tool: dict) -> str:
    name = tool.get("name")
    description = tool.get("description")
    schema = tool.get("inputSchema")
    if not isinstance(name, str) or not name:
        raise RuntimeError("tools/list returned a tool without a valid name")
    if not isinstance(description, str) or not description:
        raise RuntimeError(f"{name}: missing description")
    if not isinstance(schema, dict) or schema.get("type") != "object":
        raise RuntimeError(f"{name}: inputSchema must be an object schema")

    meta = tool.get("_meta")
    if not isinstance(meta, dict):
        raise RuntimeError(f"{name}: missing _meta object")
    result_contract = meta.get("lemonade/result")
    if not isinstance(result_contract, dict):
        raise RuntimeError(f"{name}: missing _meta.lemonade/result contract")
    result_description = result_contract.get("description")
    if not isinstance(result_description, str) or not result_description.strip():
        raise RuntimeError(f"{name}: missing resultDescription")

    lines = [
        f"### `{name}`",
        "",
        md_text(description),
        "",
        "**Returns**",
        "",
        md_text(result_description),
        "",
    ]
    annotations = tool.get("annotations")
    if annotations is not None:
        if not isinstance(annotations, dict):
            raise RuntimeError(f"{name}: annotations must be an object")
        lines += [
            "**MCP annotations**",
            "",
            "```json",
            json.dumps(annotations, indent=2, ensure_ascii=False, sort_keys=True),
            "```",
            "",
        ]

    properties = schema.get("properties", {})
    if not isinstance(properties, dict):
        raise RuntimeError(f"{name}: inputSchema.properties must be an object")
    required = set(schema.get("required", []))

    if properties:
        lines += [
            "| Argument | Required | Schema | Description |",
            "|---|:---:|---|---|",
        ]
        for arg, arg_schema in properties.items():
            if not isinstance(arg_schema, dict):
                raise RuntimeError(f"{name}.{arg}: property schema must be an object")
            arg_description = arg_schema.get("description")
            if not isinstance(arg_description, str) or not arg_description.strip():
                raise RuntimeError(f"{name}.{arg}: missing description")
            desc = md_cell(arg_description)
            shape = md_cell(f"`{compact_schema(arg_schema)}`")
            lines.append(
                f"| `{md_cell(arg)}` | {'yes' if arg in required else 'no'} | {shape} | {desc} |"
            )
        lines.append("")
    else:
        lines += ["This tool takes no arguments.", ""]

    return "\n".join(lines).rstrip()


def render(tools: list[dict]) -> str:
    names = [tool.get("name") for tool in tools]
    if len(set(names)) != len(names):
        raise RuntimeError("tools/list contains duplicate tool names")
    body = "\n\n".join(render_tool(tool) for tool in tools)
    return (
        f"{BEGIN}\n"
        "<!-- Generated from live /mcp tools/list. Do not edit this region by hand. -->\n\n"
        f"{body}\n\n"
        f"{END}"
    )


def replace_region(original: str, generated: str) -> str:
    start = original.find(BEGIN)
    end = original.find(END)
    if start < 0 or end < 0 or end < start:
        raise RuntimeError(f"Generated markers are missing from {TARGET_DOC}")
    if original.find(BEGIN, start + 1) >= 0 or original.find(END, end + 1) >= 0:
        raise RuntimeError(f"Generated markers are duplicated in {TARGET_DOC}")
    end += len(END)
    return original[:start] + generated + original[end:]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lemond", help="Path to an already-built lemond binary")
    parser.add_argument(
        "--check", action="store_true", help="Fail if committed docs are stale"
    )
    args = parser.parse_args()

    binary = find_lemond(args.lemond)
    with Lemond(binary) as server:
        generated = render(tools_list(server.port))

    original = TARGET_DOC.read_text(encoding="utf-8")
    updated = replace_region(original, generated)
    if updated == original:
        print("MCP reference docs are up to date.")
        return 0

    if args.check:
        diff = "".join(
            difflib.unified_diff(
                original.splitlines(keepends=True),
                updated.splitlines(keepends=True),
                fromfile=str(TARGET_DOC),
                tofile=f"{TARGET_DOC} (generated)",
            )
        )
        sys.stderr.write(diff)
        sys.stderr.write(
            "\nMCP reference docs are stale. Run gen_mcp_boilerplate.py.\n"
        )
        return 1

    TARGET_DOC.write_text(updated, encoding="utf-8", newline="\n")
    print(f"Updated {TARGET_DOC.relative_to(REPO_ROOT)}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)

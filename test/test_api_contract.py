#!/usr/bin/env python3
"""Check the public route contract against a running lemond, without inference.

HTTP route methods, paths, aliases and prefix families come from the real
RouteRegistry via GET /internal/routes. Probe each protected HTTP version prefix
with GET /health and each protected WebSocket URL with a real upgrade. Do not
invoke arbitrary HTTP handlers: some mutate server state or require a model.

This guards the route surface, not schemas, responses or inference behavior.
New HTTP route specs require a reviewed manifest update. Newly supported global
prefixes and WebSocket paths must be added explicitly to the manifest; the live
probes protect the listed aliases, rather than discovering unknown URLs.

Usage (against a running, rebuilt server):
    python test/test_api_contract.py --base-url http://localhost:13305
"""

import argparse
import asyncio
import base64
import json
import os
import re
from pathlib import Path
from urllib.parse import urlsplit

import requests
import websockets


MANIFEST = Path(__file__).with_name("api_contract_manifest.json")


def normalize(path: str) -> str:
    # Parameter names are not part of the URL contract. Keep trailing slashes.
    return re.sub(r"\{[^{}]+\}", "{param}", path)


def check_http_contract(manifest: dict, specs: list) -> int:
    expected = {
        (
            item["method"],
            "Quad" if "versioned_path" in item else "Root",
            normalize(item.get("versioned_path", item.get("path", ""))),
        )
        for item in manifest["http_routes"]
    }
    if not isinstance(specs, list) or not specs:
        raise AssertionError("/internal/routes did not return a nonempty route list")

    actual = set()
    for spec in specs:
        family = spec["prefixes"]
        if family == "Internal":
            continue
        if family not in {"Quad", "Root"}:
            raise AssertionError(f"Unknown prefix family {family!r}: {spec['id']}")
        for method in spec["methods"]:
            for path in spec["paths"]:
                actual.add((method, family, normalize(path)))

    missing = expected - actual
    unlisted = actual - expected
    if missing or unlisted:
        problems = ["Public HTTP API contract changed:"]
        if missing:
            problems.append("Previously protected routes are missing:")
            problems.extend(f"  {m:7} {f:4} {p}" for m, f, p in sorted(missing))
        if unlisted:
            problems.append("New public routes are not protected:")
            problems.extend(f"  {m:7} {f:4} {p}" for m, f, p in sorted(unlisted))
        problems.append("Review the changes to api_contract_manifest.json explicitly.")
        raise AssertionError("\n".join(problems))
    return len(expected)


def websocket_paths(manifest: dict) -> set[str]:
    paths = set()
    for item in manifest["websocket_routes"]:
        if item["method"] != "WS":
            raise AssertionError(f"Invalid WebSocket contract entry: {item}")
        if "path" in item:
            paths.add(item["path"])
        else:
            paths.update(
                f"{prefix}/{item['versioned_path']}"
                for prefix in manifest["versioned_prefixes"]
            )
    return paths


async def check_websocket_contract(
    ws_base: str, paths: set[str], headers: dict
) -> None:
    # Match the credential-subprotocol flow used by server_websocket_auth.py.
    protocols = ["lemonade-realtime"]
    authorization = headers.get("Authorization", "")
    if authorization.startswith("Bearer "):
        token = authorization[len("Bearer ") :]
        encoded = base64.urlsafe_b64encode(token.encode()).decode().rstrip("=")
        protocols.append(f"bearer.{encoded}")
    for path in sorted(paths):
        try:
            async with websockets.connect(
                ws_base + path,
                subprotocols=protocols,
                open_timeout=10,
                close_timeout=2,
            ) as ws:
                if ws.subprotocol != protocols[0]:
                    raise AssertionError("Unexpected WebSocket subprotocol")
        except Exception as exc:
            raise AssertionError(f"Protected WebSocket route failed: {path}") from exc


def check_api_contract(base_url: str, headers: dict) -> None:
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    if manifest["version"] != 1:
        raise AssertionError("Unsupported API contract manifest version")
    base_url = base_url.rstrip("/")
    parsed = urlsplit(base_url)
    if parsed.scheme not in {"http", "https"} or not parsed.netloc or parsed.path:
        raise ValueError("Expected an HTTP(S) server origin without a path")
    ws_scheme = "wss" if parsed.scheme == "https" else "ws"

    response = requests.get(base_url + "/internal/routes", headers=headers, timeout=30)
    response.raise_for_status()
    count = check_http_contract(manifest, response.json())

    # /health is a harmless Quad route. A missing compatibility prefix must fail
    # even if the RouteSpec still declares Prefixes::Quad.
    for prefix in manifest["versioned_prefixes"]:
        path = prefix + "/health"
        response = requests.get(
            base_url + path, headers=headers, timeout=10, allow_redirects=False
        )
        if response.status_code != 200 or not isinstance(
            response.json().get("all_models_loaded"), list
        ):
            raise AssertionError(f"Protected HTTP version prefix failed: {path}")

    paths = websocket_paths(manifest)
    asyncio.run(
        check_websocket_contract(f"{ws_scheme}://{parsed.netloc}", paths, headers)
    )
    print(f"API contract passed: {count} HTTP route specs, {len(paths)} WS URLs")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default="http://localhost:13305")
    args = parser.parse_args()
    key = os.environ.get("LEMONADE_ADMIN_API_KEY") or os.environ.get(
        "LEMONADE_API_KEY"
    )
    headers = {"Authorization": f"Bearer {key}"} if key else {}
    check_api_contract(args.base_url, headers)


if __name__ == "__main__":
    main()

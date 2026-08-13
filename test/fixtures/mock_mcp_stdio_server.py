#!/usr/bin/env python3
"""Deterministic stdio MCP fixture for the Lemonade MCP client tests."""

import json
import os
import sys
import time

PROTOCOL_VERSION = "2025-11-25"

TOOLS = [
    {
        "name": "echo",
        "title": "Echo",
        "description": "Echo back a message",
        "inputSchema": {
            "type": "object",
            "properties": {"message": {"type": "string"}},
            "required": ["message"],
        },
    },
    {
        "name": "sleep",
        "title": "Sleep",
        "description": "Sleep long enough to exercise cancellation",
        "inputSchema": {
            "type": "object",
            "properties": {"seconds": {"type": "number"}},
        },
    },
    {
        "name": "exit",
        "title": "Exit",
        "description": "Exit without a response to exercise process death",
        "inputSchema": {"type": "object", "properties": {}},
    },
]


def send(message):
    sys.stdout.write(json.dumps(message, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def reply(request_id, result):
    send({"jsonrpc": "2.0", "id": request_id, "result": result})


def error(request_id, code, message):
    send(
        {
            "jsonrpc": "2.0",
            "id": request_id,
            "error": {"code": code, "message": message},
        }
    )


for line in sys.stdin:
    if not line.strip():
        continue

    try:
        message = json.loads(line)
    except json.JSONDecodeError as exc:
        print(f"invalid request JSON: {exc}", file=sys.stderr, flush=True)
        continue

    method = message.get("method")
    request_id = message.get("id")

    if method == "initialize":
        delay = float(os.environ.get("LEMONADE_MCP_INIT_DELAY", "0"))
        if delay > 0:
            time.sleep(min(delay, 60.0))
        requested = message.get("params", {}).get("protocolVersion")
        negotiated = requested if requested == PROTOCOL_VERSION else PROTOCOL_VERSION
        reply(
            request_id,
            {
                "protocolVersion": negotiated,
                "capabilities": {"tools": {"listChanged": False}},
                "serverInfo": {"name": "mock-mcp", "version": "0.2"},
            },
        )
    elif method == "notifications/initialized":
        continue
    elif method == "notifications/cancelled":
        continue
    elif method == "tools/list":
        reply(request_id, {"tools": TOOLS})
    elif method == "tools/call":
        params = message.get("params", {})
        name = params.get("name")
        arguments = params.get("arguments", {})

        if name == "echo":
            text = arguments.get("message", "")
            secret = os.environ.get("LEMONADE_MCP_TEST_SECRET", "")
            reply(
                request_id,
                {
                    "content": [{"type": "text", "text": text}],
                    "structuredContent": {"secret_seen": bool(secret)},
                    "isError": False,
                },
            )
        elif name == "sleep":
            seconds = float(arguments.get("seconds", 10.0))
            time.sleep(max(0.0, min(seconds, 60.0)))
            reply(
                request_id,
                {
                    "content": [{"type": "text", "text": "finished"}],
                    "isError": False,
                },
            )
        elif name == "exit":
            sys.exit(0)
        else:
            reply(
                request_id,
                {
                    "content": [{"type": "text", "text": "unknown tool"}],
                    "isError": True,
                },
            )
    elif request_id is not None:
        error(request_id, -32601, f"Unknown method: {method}")

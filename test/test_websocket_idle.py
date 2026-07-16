#!/usr/bin/env python3
"""
Regression test: verify that WebSocket connections (/realtime, /logs/stream)
survive idle periods without being killed by the front server's read timeout.

The front listeners have a 30s read timeout to guard against slow-loris attacks.
WebSocket connections are accepted by the front, then adopted by libwebsockets.
This test confirms that the timeout doesn't kill idle WebSocket sessions after
adoption.

Usage:
    python3 test_websocket_idle.py [port] [idle_seconds]

    Default: port from LEMONADE_TEST_PORT (13305), idle from LEMONADE_WS_IDLE_SECONDS (5)
    For full regression: LEMONADE_WS_IDLE_SECONDS=31 (proves >30s survival)
"""

import asyncio
import os
import sys

import websockets


async def test_idle_websocket(port: int, idle_seconds: int):
    """Hold a WebSocket connection idle, then verify it's still alive."""
    uri = f"ws://localhost:{port}/logs/stream"
    print(f"Connecting to {uri}...")

    async with websockets.connect(uri) as ws:
        print(f"Connected. Waiting {idle_seconds}s idle...")
        await asyncio.sleep(idle_seconds)

        print("Sending ping...")
        pong_waiter = await ws.ping()
        await asyncio.wait_for(pong_waiter, timeout=5)
        print(f"✓ Pong received — connection alive after {idle_seconds}s idle")


if __name__ == "__main__":
    port = (
        int(sys.argv[1])
        if len(sys.argv) > 1
        else int(os.environ.get("LEMONADE_TEST_PORT", "13305"))
    )
    idle_seconds = (
        int(sys.argv[2])
        if len(sys.argv) > 2
        else int(os.environ.get("LEMONADE_WS_IDLE_SECONDS", "5"))
    )
    asyncio.run(test_idle_websocket(port, idle_seconds))

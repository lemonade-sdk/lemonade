#!/usr/bin/env python

import os
import asyncio
import websockets

connections = set()


async def echo(websocket, path):

    connections.add(websocket)

    try:
        async for message in websocket:
            print(message)

            for ws in connections:
                if ws != websocket:
                    await ws.send(message)
    except:
        raise
    finally:
        connections.remove(websocket)


asyncio.get_event_loop().run_until_complete(websockets.serve(echo, "localhost", 8080))
asyncio.get_event_loop().run_forever()

#!/usr/bin/env python3
"""A stand-in engine for the adapter plane's live wiring test: records the argv and environment it was
started with (to $LEMONADE_ENGINE_RECORD), then serves /health, /v1/models and a one-line chat.
"""

import json, os, sys
from http.server import BaseHTTPRequestHandler, HTTPServer

port = int(sys.argv[sys.argv.index("--port") + 1])
with open(os.environ["LEMONADE_ENGINE_RECORD"], "w") as f:
    json.dump(
        {
            "argv": sys.argv[1:],
            "env": {
                k: v
                for k, v in os.environ.items()
                if k.startswith(
                    (
                        "CUDA_",
                        "GGML_",
                        "LEMONADE_ENGINE_",
                        "LD_LIBRARY_PATH",
                        "LD_PRELOAD",
                        "STRATA_",
                    )
                )
            },
        },
        f,
    )


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _j(self, obj):
        b = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("content-type", "application/json")
        self.send_header("content-length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        if self.path.startswith("/memory"):  # R9: memory by device and tier
            return self._j(
                {
                    "devices": [
                        {"uuid": "GPU-fake", "bytes": 1048576, "evictable": True}
                    ],
                    "pinned_host": 2048,
                    "remote": 4096,
                    "disk": 8192,
                }
            )
        self._j(
            {"status": "ok"}
            if self.path.startswith("/health")
            else {
                "object": "list",
                "data": [{"id": "fake", "object": "model", "owned_by": "fake"}],
            }
        )

    def do_POST(self):
        n = int(self.headers.get("content-length") or 0)
        self.rfile.read(n)
        self._j(
            {
                "id": "x",
                "object": "chat.completion",
                "model": "fake",
                "choices": [
                    {
                        "index": 0,
                        "finish_reason": "stop",
                        "message": {
                            "role": "assistant",
                            "content": "fake engine answer",
                        },
                    }
                ],
            }
        )


HTTPServer(("127.0.0.1", port), H).serve_forever()

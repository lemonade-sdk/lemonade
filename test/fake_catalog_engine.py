#!/usr/bin/env python3
import argparse
import http.server
import json
from pathlib import Path
import sys
import threading


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--batch", type=int, default=2)
    parser.add_argument("--tag", default="initial")
    parser.add_argument("--model-fixture", required=True)
    args = parser.parse_args()
    trace = Path(args.model_fixture).with_suffix(".observations.jsonl")
    lock = threading.Lock()

    def record(event):
        with lock, trace.open("a") as output:
            output.write(json.dumps(event, sort_keys=True) + "\n")

    record({"event": "argv", "argv": sys.argv[1:]})

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def reply(self, body, content_type):
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            self.reply(b'{"status":"ok"}', "application/json")

        def do_POST(self):
            request = json.loads(
                self.rfile.read(int(self.headers.get("Content-Length", "0")))
            )
            record({"event": "request", "path": self.path, "body": request})
            text = f"fake batch={args.batch} tag={args.tag}"
            if request.get("stream"):
                events = [
                    {
                        "id": "fake",
                        "object": "chat.completion.chunk",
                        "choices": [
                            {
                                "index": 0,
                                "delta": {"role": "assistant", "content": text},
                                "finish_reason": None,
                            }
                        ],
                    },
                    {
                        "id": "fake",
                        "object": "chat.completion.chunk",
                        "choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}],
                        "usage": {
                            "prompt_tokens": 1,
                            "completion_tokens": 1,
                            "total_tokens": 2,
                        },
                    },
                ]
                raw = "".join("data: " + json.dumps(event) + "\n\n" for event in events)
                self.reply((raw + "data: [DONE]\n\n").encode(), "text/event-stream")
            else:
                self.reply(
                    json.dumps(
                        {
                            "id": "fake",
                            "object": "chat.completion",
                            "choices": [
                                {
                                    "index": 0,
                                    "message": {"role": "assistant", "content": text},
                                    "finish_reason": "stop",
                                }
                            ],
                            "usage": {
                                "prompt_tokens": 1,
                                "completion_tokens": 1,
                                "total_tokens": 2,
                            },
                        }
                    ).encode(),
                    "application/json",
                )

    http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()

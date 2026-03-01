"""
Omni Endpoint — Custom Tools with Callback

Defines custom tools (query_orders, issue_refund) and handles them
via a local callback server. The LLM sees your tools alongside
built-in ones and calls them as needed.

Your callback server receives:
    {"tool_call_id": "abc", "tool_name": "query_orders", "arguments": {"order_id": 4521}}

And responds with:
    {"success": true, "result": {...}, "summary": "Human-readable summary for the LLM"}

Usage:
    python examples/omni_custom_tools.py
"""

import json
import threading
from http.server import HTTPServer, BaseHTTPRequestHandler

import requests

LEMONADE_URL = "http://localhost:8000"
CALLBACK_PORT = 9000


# --- Callback server (handles your custom tools) ---


class ToolHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        name = body["tool_name"]
        args = body["arguments"]

        if name == "query_orders":
            result = {"order_id": args["order_id"], "status": "shipped", "eta": "March 3"}
            summary = f"Order #{args['order_id']}: shipped, ETA March 3"
        elif name == "issue_refund":
            result = {"refund_id": "R-9921", "amount": args["amount"]}
            summary = f"Refund of ${args['amount']} issued (R-9921)"
        else:
            result = {"error": f"Unknown tool: {name}"}
            summary = f"Unknown tool: {name}"

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(
            json.dumps({"success": True, "result": result, "summary": summary}).encode()
        )

    def log_message(self, *args):
        pass


server = HTTPServer(("localhost", CALLBACK_PORT), ToolHandler)
threading.Thread(target=server.serve_forever, daemon=True).start()
print(f"Callback server running on :{CALLBACK_PORT}")


# --- Omni request with custom tools ---


response = requests.post(
    f"{LEMONADE_URL}/v1/omni/chat",
    json={
        "model": "gpt-oss-20b-mxfp4-GGUF",
        "messages": [
            {
                "role": "user",
                "content": "My order #4521 arrived damaged. Can I get a refund for $29.99?",
            }
        ],
        "stream": False,
        "omni": {
            "system_prompt": (
                "You are a customer support agent for an online store. "
                "Use query_orders to look up order details. "
                "Use issue_refund to process refunds. "
                "Be empathetic and professional."
            ),
            "tools": [],  # no built-in tools needed
            "extra_tools": [
                {
                    "type": "function",
                    "function": {
                        "name": "query_orders",
                        "description": "Look up an order by ID",
                        "parameters": {
                            "type": "object",
                            "properties": {
                                "order_id": {
                                    "type": "integer",
                                    "description": "The order ID",
                                }
                            },
                            "required": ["order_id"],
                        },
                    },
                },
                {
                    "type": "function",
                    "function": {
                        "name": "issue_refund",
                        "description": "Issue a refund for a given amount",
                        "parameters": {
                            "type": "object",
                            "properties": {
                                "amount": {
                                    "type": "number",
                                    "description": "Refund amount in USD",
                                }
                            },
                            "required": ["amount"],
                        },
                    },
                },
            ],
            "tool_callback_url": f"http://localhost:{CALLBACK_PORT}",
            "tool_callback_timeout": 10,
        },
    },
)

data = response.json()

print()
print("=== Tool Calls ===")
for step in data["omni_steps"]:
    for result in step["results"]:
        print(f"  [{result['tool_name']}] {result['summary']}")

print()
print("=== Response ===")
print(data["choices"][0]["message"]["content"])

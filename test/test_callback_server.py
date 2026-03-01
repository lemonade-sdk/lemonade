"""Minimal callback server for testing omni external tools."""

from http.server import HTTPServer, BaseHTTPRequestHandler
import json


class CallbackHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        data = json.loads(body)

        print(f"\n=== Tool Called ===")
        print(f"  tool_name:    {data.get('tool_name')}")
        print(f"  tool_call_id: {data.get('tool_call_id')}")
        print(f"  arguments:    {json.dumps(data.get('arguments', {}))}")
        print(f"==================\n")

        response = json.dumps({
            "success": True,
            "result": {"answer": 42},
            "summary": f"Tool {data['tool_name']} executed successfully. The answer is 42."
        })

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(response.encode())

    def log_message(self, format, *args):
        print(f"[callback-server] {args[0]}")


if __name__ == "__main__":
    server = HTTPServer(("localhost", 9000), CallbackHandler)
    print("[callback-server] Listening on http://localhost:9000")
    server.serve_forever()

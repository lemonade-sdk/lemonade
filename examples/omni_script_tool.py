"""
Omni Endpoint — Script-Based Custom Tool

Defines a custom search tool backed by a local Python script.
No callback server needed — Lemonade spawns the script, pipes the
tool call as JSON to stdin, and reads the JSON result from stdout.

The script follows the same JSON contract as the callback server:

  Input (stdin):
    {"tool_call_id": "abc", "tool_name": "my_search", "arguments": {"query": "..."}}

  Output (stdout):
    {"success": true, "result": {...}, "summary": "Human-readable summary"}

Usage:
    python examples/omni_script_tool.py
"""

import requests

LEMONADE_URL = "http://localhost:8000"

response = requests.post(
    f"{LEMONADE_URL}/v1/omni/chat",
    json={
        "model": "gpt-oss-20b-mxfp4-GGUF",
        "messages": [
            {
                "role": "user",
                "content": "Search for 'local LLM inference' using the my_search tool.",
            }
        ],
        "stream": False,
        "omni": {
            "system_prompt": (
                "You are a helpful assistant with access to a search tool. "
                "When asked to search, use the my_search tool."
            ),
            "tools": [],  # no built-in tools needed
            "extra_tools": [
                {
                    "type": "function",
                    "function": {
                        "name": "my_search",
                        "description": "Search for information using a custom search engine",
                        "parameters": {
                            "type": "object",
                            "properties": {
                                "query": {
                                    "type": "string",
                                    "description": "The search query",
                                }
                            },
                            "required": ["query"],
                        },
                    },
                    "script": "python examples/my_search.py",
                }
            ],
        },
    },
)

data = response.json()

print()
print("=== Tool Calls ===")
for step in data.get("omni_steps", []):
    for result in step.get("results", []):
        print(f"  [{result['tool_name']}] {result['summary']}")

print()
print("=== Response ===")
print(data["choices"][0]["message"]["content"])

"""
Omni Endpoint — Basic Usage

Uses the omni endpoint as-is with built-in tools.
The LLM autonomously picks which tools to call.

Usage:
    python examples/omni_basic.py
"""

import requests

LEMONADE_URL = "http://localhost:8000"

response = requests.post(f"{LEMONADE_URL}/v1/omni/chat", json={
    "model": "gpt-oss-20b-mxfp4-GGUF",
    "messages": [
        {"role": "user", "content": "What files are on my Desktop? List them."}
    ],
    "stream": False,
    "omni": {
        "tools": ["list_directory", "run_command"],
        "max_iterations": 5,
    },
})

data = response.json()

print("=== Tool Calls ===")
for step in data["omni_steps"]:
    for result in step["results"]:
        print(f"  [{result['tool_name']}] {result['summary'][:100]}")

print()
print("=== Response ===")
print(data["choices"][0]["message"]["content"])

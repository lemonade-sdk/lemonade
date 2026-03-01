"""
Omni Endpoint — Custom System Prompt

Overrides the default system prompt to create a focused research assistant
that always searches the web before answering.

Usage:
    python examples/omni_custom_prompt.py
"""

import requests

LEMONADE_URL = "http://localhost:8000"

response = requests.post(f"{LEMONADE_URL}/v1/omni/chat", json={
    "model": "gpt-oss-20b-mxfp4-GGUF",
    "messages": [
        {"role": "user", "content": "What's the weather like in Portland?"}
    ],
    "stream": False,
    "omni": {
        "system_prompt": (
            "You are a concise research assistant. "
            "Always use web_search to find current information before answering. "
            "Cite your sources. Never make up facts."
        ),
        "tools": ["web_search", "read_file", "write_file"],
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

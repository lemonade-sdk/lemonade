"""
Example script tool for the Omni endpoint.

Protocol:
  - Reads JSON from stdin: {"tool_call_id": "...", "tool_name": "...", "arguments": {...}}
  - Writes JSON to stdout: {"success": true, "result": {...}, "summary": "..."}

This is a stub that returns dummy search results. Replace with your own logic
(call an API, query a database, run a local search index, etc.).

Usage:
    echo '{"tool_call_id":"1","tool_name":"my_search","arguments":{"query":"test"}}' | python examples/my_search.py
"""

import json
import sys


def main():
    data = json.load(sys.stdin)
    query = data["arguments"].get("query", "")

    # --- Replace this with your actual search logic ---
    results = [
        {"title": f"Result 1 for '{query}'", "snippet": "This is a dummy result."},
        {"title": f"Result 2 for '{query}'", "snippet": "Another dummy result."},
    ]

    response = {
        "success": True,
        "result": {"query": query, "results": results},
        "summary": f"Found {len(results)} results for '{query}'.",
    }

    print(json.dumps(response))


if __name__ == "__main__":
    main()

"""Simple client example for Lemonade Lean server."""

import requests
import json

# Server URL
BASE_URL = "http://localhost:8000"


def chat_example():
    """Example of chat completion."""
    print("=== Chat Completion Example ===")

    response = requests.post(
        f"{BASE_URL}/v1/chat/completions",
        json={
            "messages": [{"role": "user", "content": "What is the capital of France?"}],
            "temperature": 0.7,
            "max_tokens": 100,
        },
    )

    result = response.json()
    print("Response:", json.dumps(result, indent=2))
    print()


def completion_example():
    """Example of text completion."""
    print("=== Text Completion Example ===")

    response = requests.post(
        f"{BASE_URL}/v1/completions",
        json={"prompt": "Once upon a time", "temperature": 0.7, "max_tokens": 50},
    )

    result = response.json()
    print("Response:", json.dumps(result, indent=2))
    print()


def streaming_example():
    """Example of streaming chat completion."""
    print("=== Streaming Chat Example ===")

    response = requests.post(
        f"{BASE_URL}/v1/chat/completions",
        json={
            "messages": [{"role": "user", "content": "Tell me a short joke"}],
            "stream": True,
            "max_tokens": 100,
        },
        stream=True,
    )

    print("Streaming response: ", end="", flush=True)
    for line in response.iter_lines():
        if line:
            line_str = line.decode("utf-8")
            if line_str.startswith("data: "):
                data = line_str[6:]
                if data != "[DONE]":
                    try:
                        chunk = json.loads(data)
                        if "choices" in chunk and len(chunk["choices"]) > 0:
                            delta = chunk["choices"][0].get("delta", {})
                            if "content" in delta:
                                print(delta["content"], end="", flush=True)
                    except json.JSONDecodeError:
                        pass
    print("\n")


def health_check():
    """Check server health."""
    print("=== Health Check ===")

    response = requests.get(f"{BASE_URL}/v1/health")
    result = response.json()
    print("Health:", json.dumps(result, indent=2))
    print()


if __name__ == "__main__":
    try:
        health_check()
        chat_example()
        completion_example()
        streaming_example()
    except requests.exceptions.ConnectionError:
        print("Error: Could not connect to server. Make sure the server is running.")
    except Exception as e:
        print(f"Error: {e}")

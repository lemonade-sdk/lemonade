#!/usr/bin/env python3
"""
Simple test script for Ryzen AI LLM Server
Tests the health, completion, and chat endpoints
"""

import requests
import json
import sys
import time

SERVER_URL = "http://localhost:8080"


def test_health():
    """Test the health endpoint"""
    print("\n" + "=" * 60)
    print("Testing /health endpoint...")
    print("=" * 60)

    try:
        response = requests.get(f"{SERVER_URL}/health")
        response.raise_for_status()
        data = response.json()

        print("✓ Health check passed!")
        print(f"  Status: {data['status']}")
        print(f"  Model: {data['model']}")
        print(f"  Execution mode: {data['execution_mode']}")
        print(f"  Max prompt length: {data['max_prompt_length']}")
        print(f"  Ryzen AI version: {data['ryzenai_version']}")
        return True
    except Exception as e:
        print(f"✗ Health check failed: {e}")
        return False


def test_completion():
    """Test the completion endpoint"""
    print("\n" + "=" * 60)
    print("Testing /v1/completions endpoint...")
    print("=" * 60)

    try:
        payload = {
            "prompt": "The quick brown fox",
            "max_tokens": 20,
            "temperature": 0.7,
        }

        print(f"Prompt: '{payload['prompt']}'")
        print("Waiting for response...")

        start_time = time.time()
        response = requests.post(
            f"{SERVER_URL}/v1/completions",
            headers={"Content-Type": "application/json"},
            json=payload,
        )
        response.raise_for_status()
        duration = time.time() - start_time

        data = response.json()
        completion = data["choices"][0]["text"]

        print(f"✓ Completion successful! (took {duration:.2f}s)")
        print(f"  Response: {completion}")
        return True
    except Exception as e:
        print(f"✗ Completion failed: {e}")
        return False


def test_chat():
    """Test the chat completion endpoint"""
    print("\n" + "=" * 60)
    print("Testing /v1/chat/completions endpoint...")
    print("=" * 60)

    try:
        payload = {
            "messages": [
                {"role": "user", "content": "Say 'Hello World' and nothing else"}
            ],
            "max_tokens": 30,
            "temperature": 0.7,
        }

        print(f"User message: '{payload['messages'][0]['content']}'")
        print("Waiting for response...")

        start_time = time.time()
        response = requests.post(
            f"{SERVER_URL}/v1/chat/completions",
            headers={"Content-Type": "application/json"},
            json=payload,
        )
        response.raise_for_status()
        duration = time.time() - start_time

        data = response.json()
        message = data["choices"][0]["message"]["content"]

        print(f"✓ Chat completion successful! (took {duration:.2f}s)")
        print(f"  Assistant: {message}")
        return True
    except Exception as e:
        print(f"✗ Chat completion failed: {e}")
        return False


def test_streaming():
    """Test streaming completion"""
    print("\n" + "=" * 60)
    print("Testing /v1/chat/completions endpoint (streaming)...")
    print("=" * 60)

    try:
        payload = {
            "messages": [{"role": "user", "content": "Count from 1 to 5"}],
            "max_tokens": 50,
            "stream": True,
        }

        print(f"User message: '{payload['messages'][0]['content']}'")
        print("Streaming response: ", end="", flush=True)

        response = requests.post(
            f"{SERVER_URL}/v1/chat/completions",
            headers={"Content-Type": "application/json"},
            json=payload,
            stream=True,
        )
        response.raise_for_status()

        tokens = []
        for line in response.iter_lines():
            if line:
                line = line.decode("utf-8")
                if line.startswith("data: "):
                    data_str = line[6:]
                    if data_str == "[DONE]":
                        break
                    try:
                        data = json.loads(data_str)
                        if "choices" in data and len(data["choices"]) > 0:
                            delta = data["choices"][0].get("delta", {})
                            content = delta.get("content", "")
                            if content:
                                print(content, end="", flush=True)
                                tokens.append(content)
                    except json.JSONDecodeError:
                        pass

        print()  # New line
        print(f"✓ Streaming successful! Received {len(tokens)} chunks")
        return True
    except Exception as e:
        print(f"\n✗ Streaming failed: {e}")
        return False


def main():
    """Run all tests"""
    print("\n" + "=" * 60)
    print("Ryzen AI LLM Server Test Suite")
    print("=" * 60)
    print(f"\nServer URL: {SERVER_URL}")
    print("\nMake sure the server is running with:")
    print("  ryzenai-serve.exe -m <model_path>")

    input("\nPress Enter to start tests...")

    results = []

    # Test health endpoint
    results.append(("Health", test_health()))
    time.sleep(1)

    # Test completion endpoint
    results.append(("Completion", test_completion()))
    time.sleep(1)

    # Test chat endpoint
    results.append(("Chat", test_chat()))
    time.sleep(1)

    # Test streaming
    results.append(("Streaming", test_streaming()))

    # Summary
    print("\n" + "=" * 60)
    print("Test Summary")
    print("=" * 60)

    passed = sum(1 for _, result in results if result)
    total = len(results)

    for test_name, result in results:
        status = "✓ PASS" if result else "✗ FAIL"
        print(f"  {test_name:20s} {status}")

    print()
    print(f"Total: {passed}/{total} tests passed")
    print("=" * 60)

    if passed == total:
        print("\n🎉 All tests passed! Server is working correctly.")
        return 0
    else:
        print(f"\n⚠️  {total - passed} test(s) failed. Check the output above.")
        return 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n\nTests interrupted by user.")
        sys.exit(130)
    except Exception as e:
        print(f"\n\nUnexpected error: {e}")
        sys.exit(1)

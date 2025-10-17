from openai import OpenAI

# Initialize client
client = OpenAI(base_url="http://localhost:8000/api/v0", api_key="not-needed")

print("🧠 Testing enable_thinking parameter...")
print("=" * 50)

# Test 1: enable_thinking=True (should show thinking)
print("Test 1: enable_thinking=True (thinking mode)")
try:
    completion = client.chat.completions.create(
        model="Qwen3-4B-GGUF",  # Make sure you have this model
        messages=[{"role": "user", "content": "What is 15 * 23?"}],
        max_completion_tokens=100,
        extra_body={"enable_thinking": True},
    )
    print("Response with thinking:")
    print(completion.choices[0].message.content)
    print()
except Exception as e:
    print(f"Error in Test 1: {e}")
    print()

# Test 2: enable_thinking=False (should be fast, no thinking)
print("Test 2: enable_thinking=False (fast mode)")
try:
    completion = client.chat.completions.create(
        model="Qwen3-4B-GGUF",
        messages=[{"role": "user", "content": "What is 15 * 23?"}],
        max_completion_tokens=100,
        extra_body={"enable_thinking": False},
    )
    print("Response without thinking:")
    print(completion.choices[0].message.content)
    print()
except Exception as e:
    print(f"Error in Test 2: {e}")
    print()

# Test 3: Default behavior (should be thinking=True)
print("Test 3: Default behavior (should default to thinking)")
try:
    completion = client.chat.completions.create(
        model="Qwen3-4B-GGUF",
        messages=[{"role": "user", "content": "What is 15 * 23?"}],
        max_completion_tokens=100,
    )
    print("Default response:")
    print(completion.choices[0].message.content)
except Exception as e:
    print(f"Error in Test 3: {e}")

print("=" * 50)
print("✅ Testing complete!")

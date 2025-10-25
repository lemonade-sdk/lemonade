# for qwen 3
from openai import OpenAI

# Connect to Lemonade
client = OpenAI(base_url="http://localhost:8000/api/v1", api_key="not-needed")

print("🧪 Testing enable_thinking with Qwen3-4B-GGUF...")
print("=" * 70)

# # Test 1: enable_thinking=True
print("\n📝 Test 1: enable_thinking=True")
print("-" * 70)
try:
    completion = client.chat.completions.create(
        model="Qwen3-4B-GGUF",  # Changed to Qwen3
        messages=[{"role": "user", "content": "What is 15 * 23?"}],
        max_tokens=500,  # Increased for reasoning content
        extra_body={"enable_thinking": True},  # Direct parameter
    )
    print("Full message object:")
    print(completion.choices[0].message)
except Exception as e:
    print(f"❌ Error: {e}")

# # Test 2: enable_thinking=False
print("\n📝 Test 2: enable_thinking=False")
print("-" * 70)
try:
    completion = client.chat.completions.create(
        model="Qwen3-4B-GGUF",
        messages=[{"role": "user", "content": "What is 15 * 23?"}],
        max_tokens=500,
        extra_body={"enable_thinking": False},
    )
    print("Full message object:")
    print(completion.choices[0].message)
except Exception as e:
    print(f"❌ Error: {e}")

print("\n" + "=" * 70)

from openai import OpenAI

c = OpenAI(base_url="http://localhost:8000/api/v1", api_key="lemonade")
for i in range(50):
    r = c.chat.completions.create(
        model="Qwen3-0.6B-GGUF",
        messages=[{"role": "user", "content": f"Count to {i}"}],
        max_completion_tokens=50,
    )
    print(f"{i}: {r.usage.completion_tokens} completion tokens.")

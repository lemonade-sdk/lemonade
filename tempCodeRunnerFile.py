print("Test 2: enable_thinking=False (fast mode)")
# try:
#     completion = client.chat.completions.create(
#         model="Qwen3-4B-GGUF",
#         messages=[{"role": "user", "content": "What is 15 * 23?"}],
#         max_completion_tokens=100,
#         extra_body={"enable_thinking": False},
#     )
#     print("Response without thinking:")
#     print(completion.choices[0].message)
#     print()
# except Exception as e:
#     print(f"Error in Test 2: {e}")
#     print()

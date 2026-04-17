"""
Lemonade Tools — agentic loop example.

Demonstrates how to use Lemonade's multimodal endpoints as tools in an
LLM agentic loop. The LLM decides which tool to call; this script
executes the tool against Lemonade's API and feeds the result back.

Prerequisites:
    pip install openai
    # Lemonade server running with an LLM + image/TTS models loaded

Usage:
    python examples/lemonade_tools.py "Generate an image of a sunset"
    python examples/lemonade_tools.py "Say hello world out loud"
"""

import json
import base64
import sys
from openai import OpenAI

# Print non-ASCII characters (emoji) without choking on Windows cp1252
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

LEMONADE_URL = "http://localhost:13305/v1"

# Edit these to match models you have installed. Defaults are small so
# they fit on most hardware.
LLM_MODEL = "Qwen3-4B-Instruct-2507-GGUF"   # any model with the "tool-calling" label
IMAGE_MODEL = "SD-Turbo"                    # any model with the "image" label
TTS_MODEL = "kokoro-v1"                     # any model with the "tts" label

# Tool definitions — same format the app uses (from toolDefinitions.json)
TOOLS = [
    {
        "type": "function",
        "function": {
            "name": "generate_image",
            "description": "Generate an image from a text description.",
            "parameters": {
                "type": "object",
                "properties": {
                    "prompt": {
                        "type": "string",
                        "description": "A detailed description of the image to generate",
                    },
                },
                "required": ["prompt"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "text_to_speech",
            "description": "Convert text to spoken audio.",
            "parameters": {
                "type": "object",
                "properties": {
                    "input": {
                        "type": "string",
                        "description": "The text to convert to speech",
                    },
                },
                "required": ["input"],
            },
        },
    },
]

SYSTEM_PROMPT = (
    "You are a helpful assistant with access to tools for generating images "
    "and converting text to speech. Use the appropriate tool when the user "
    "asks for an image or audio. After using a tool, briefly describe what "
    "you did."
)


def execute_tool(client, tool_call):
    name = tool_call.function.name
    args = json.loads(tool_call.function.arguments)

    if name == "generate_image":
        result = client.images.generate(
            model=IMAGE_MODEL,
            prompt=args["prompt"],
            response_format="b64_json",
            n=1,
        )
        image_b64 = result.data[0].b64_json
        with open("output.png", "wb") as f:
            f.write(base64.b64decode(image_b64))
        print(f"  -> Image saved to output.png ({len(image_b64)} base64 chars)")
        return "Image generated and saved to output.png."

    if name == "text_to_speech":
        audio = client.audio.speech.create(
            model=TTS_MODEL,
            input=args["input"],
            voice="af_heart",
        )
        audio.write_to_file("output.wav")
        print("  -> Audio saved to output.wav")
        return "Audio generated and saved to output.wav."

    return f"Unknown tool: {name}"


def main():
    prompt = " ".join(sys.argv[1:]) if len(sys.argv) > 1 else "Generate an image of a cat in space"
    print(f"User: {prompt}\n")

    client = OpenAI(base_url=LEMONADE_URL, api_key="not-needed")

    messages = [
        {"role": "system", "content": SYSTEM_PROMPT},
        {"role": "user", "content": prompt},
    ]

    # Agentic loop (max 3 iterations)
    for i in range(3):
        response = client.chat.completions.create(
            model=LLM_MODEL,
            messages=messages,
            tools=TOOLS,
        )

        message = response.choices[0].message

        if not message.tool_calls:
            print(f"Assistant: {message.content}")
            break

        messages.append(message)

        for tool_call in message.tool_calls:
            print(f"  [Tool] {tool_call.function.name}({tool_call.function.arguments})")
            result = execute_tool(client, tool_call)
            messages.append(
                {
                    "role": "tool",
                    "tool_call_id": tool_call.id,
                    "content": result,
                }
            )
    else:
        print("(max iterations reached)")


if __name__ == "__main__":
    main()

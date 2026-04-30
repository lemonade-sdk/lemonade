"""
OmniRouter tool calling: agentic loop example.

Demonstrates how to use Lemonade's multimodal endpoints as tools in an
LLM agentic loop. The LLM decides which tool to call; this script
executes the tool against Lemonade's API and feeds the result back.

Prerequisites:
    pip install openai

Running the Lemonade server with the models referenced below already
downloaded is easiest — install the Lite Collection from the desktop
app (Lemonade > Models > Lite Collection > Download) and you'll have
everything in one click. Otherwise, pull the models below individually
via `lemonade pull <name>`.

Usage:
    python examples/lemonade_tools.py "Generate an image of a sunset"
    python examples/lemonade_tools.py "Say hello world out loud"
"""

import json
import base64
import sys
import urllib.request
from openai import OpenAI

# Print non-ASCII characters (emoji) without choking on Windows cp1252
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

LEMONADE_URL = "http://localhost:13305/v1"

# Edit these to match models you have installed. Defaults are small so
# they fit on most hardware (and match the Lite Collection).
LLM_MODEL = "Qwen3-4B-Instruct-2507-GGUF"   # any model with the "tool-calling" label
IMAGE_MODEL = "SD-Turbo"                    # any model with the "image" label
TTS_MODEL = "kokoro-v1"                     # any model with the "tts" label

# Tool definitions — same format src/app/src/renderer/utils/toolDefinitions.json uses
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


def preflight_models():
    """Hit /v1/models?show_all=true and fail loudly if any hardcoded
    model name isn't present. Without this, the first tool call just
    returns a 404 and it's not obvious what went wrong."""
    try:
        with urllib.request.urlopen(f"{LEMONADE_URL}/models?show_all=true", timeout=5) as r:
            models = {m["id"]: m for m in json.load(r).get("data", [])}
    except Exception as e:
        print(f"Can't reach Lemonade at {LEMONADE_URL}: {e}", file=sys.stderr)
        print("Is the server running? (desktop app, or `lemond`)", file=sys.stderr)
        sys.exit(1)

    missing = [name for name in (LLM_MODEL, IMAGE_MODEL, TTS_MODEL) if name not in models]
    if missing:
        print(f"Required models not installed: {', '.join(missing)}", file=sys.stderr)
        print("Fix: open the desktop app and download the Lite Collection,", file=sys.stderr)
        print("or edit LLM_MODEL / IMAGE_MODEL / TTS_MODEL at the top of", file=sys.stderr)
        print("this script to match models you already have.", file=sys.stderr)
        sys.exit(1)


def main():
    prompt = " ".join(sys.argv[1:]) if len(sys.argv) > 1 else "Generate an image of a cat in space"
    print(f"User: {prompt}\n")

    preflight_models()

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

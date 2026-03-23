# Lemonade Tools

Lemonade exposes multimodal capabilities through standard OpenAI-compatible endpoints. You can use these as tools in any LLM agentic loop — pass the tool definitions to your LLM, execute the tool calls against Lemonade's endpoints, and feed the results back.

## Tool Definitions

The canonical tool definitions live in [`src/app/src/renderer/utils/toolDefinitions.json`](../src/app/src/renderer/utils/toolDefinitions.json). This is the single source of truth used by both the Electron app and this documentation.

Each entry has a `function` object in standard OpenAI tool-calling format (pass directly to any LLM), plus `requires_labels` or `requires_llm_labels` indicating which model capabilities are needed.

## Available Tools

| Tool | Endpoint | Request | Response |
|------|----------|---------|----------|
| `generate_image` | `POST /v1/images/generations` | JSON: `model`, `prompt`, `response_format`, `n` | `data[0].b64_json` (base64 PNG) |
| `edit_image` | `POST /v1/images/edits` | Multipart: `model`, `prompt`, `image` (file), `response_format`, `n` | `data[0].b64_json` (base64 PNG) |
| `text_to_speech` | `POST /v1/audio/speech` | JSON: `model`, `input`, `voice` | Binary audio (WAV) |
| `transcribe_audio` | `POST /v1/audio/transcriptions` | Multipart: `file`, `model`, `language` | `{"text": "..."}` |
| `analyze_image` | `POST /v1/chat/completions` | JSON: `model`, `messages` with image content | `choices[0].message.content` |

## Quick Start

### 1. Find your models

```bash
curl http://localhost:8000/v1/models
```

Model labels tell you their capabilities:

| Label | Tool |
|-------|------|
| `image` | `generate_image`, `edit_image` |
| `tts`, `speech` | `text_to_speech` |
| `audio`, `transcription` | `transcribe_audio` |
| `vision` (on LLM) | `analyze_image` |

### 2. Copy the tool definitions

From `toolDefinitions.json`, take the `function` objects and wrap them as OpenAI tools:

```python
tools = [
    {"type": "function", "function": tool["function"]}
    for tool in tool_definitions["tools"]
]
```

### 3. Run your agentic loop

```python
from openai import OpenAI
import json, base64

client = OpenAI(base_url="http://localhost:8000/v1", api_key="na")

tools = [
    {
        "type": "function",
        "function": {
            "name": "generate_image",
            "description": "Generate an image from a text description.",
            "parameters": {
                "type": "object",
                "properties": {
                    "prompt": {"type": "string", "description": "Image description"}
                },
                "required": ["prompt"]
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "text_to_speech",
            "description": "Convert text to spoken audio.",
            "parameters": {
                "type": "object",
                "properties": {
                    "input": {"type": "string", "description": "Text to speak"}
                },
                "required": ["input"]
            }
        }
    }
]

messages = [{"role": "user", "content": "Generate an image of a sunset over mountains"}]

response = client.chat.completions.create(
    model="your-llm-model",
    messages=messages,
    tools=tools
)

message = response.choices[0].message
if message.tool_calls:
    for tool_call in message.tool_calls:
        args = json.loads(tool_call.function.arguments)

        if tool_call.function.name == "generate_image":
            result = client.images.generate(
                model="SDXL-Turbo",
                prompt=args["prompt"],
                response_format="b64_json",
                n=1
            )
            image_b64 = result.data[0].b64_json
            with open("output.png", "wb") as f:
                f.write(base64.b64decode(image_b64))
            print("Image saved to output.png")

        elif tool_call.function.name == "text_to_speech":
            audio = client.audio.speech.create(
                model="kokoro-v1",
                input=args["input"],
                voice="af_heart"
            )
            audio.write_to_file("output.wav")
            print("Audio saved to output.wav")
```

## Endpoint Details

### generate_image

```bash
curl -X POST http://localhost:8000/v1/images/generations \
  -H "Content-Type: application/json" \
  -d '{"model": "SDXL-Turbo", "prompt": "a cat in space", "response_format": "b64_json", "n": 1}'
```

### edit_image

Requires `multipart/form-data`. Attach the source image as a file field named `image`:

```bash
curl -X POST http://localhost:8000/v1/images/edits \
  -F "model=SDXL-Turbo" \
  -F "prompt=make it nighttime" \
  -F "image=@source.png" \
  -F "response_format=b64_json" \
  -F "n=1"
```

### text_to_speech

```bash
curl -X POST http://localhost:8000/v1/audio/speech \
  -H "Content-Type: application/json" \
  -d '{"model": "kokoro-v1", "input": "Hello world", "voice": "af_heart"}' \
  --output speech.wav
```

### transcribe_audio

```bash
curl -X POST http://localhost:8000/v1/audio/transcriptions \
  -F "file=@audio.wav" \
  -F "model=Whisper-Large-v3-Turbo" \
  -F "language=en"
```

### analyze_image

```bash
curl -X POST http://localhost:8000/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model": "your-vision-model",
    "messages": [{"role": "user", "content": [
      {"type": "image_url", "image_url": {"url": "data:image/png;base64,..."}},
      {"type": "text", "text": "What is in this image?"}
    ]}],
    "stream": false
  }'
```

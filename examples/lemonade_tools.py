"""
Lemonade Omni Models: tool calling agentic loop example.

Demonstrates how to use Lemonade's multimodal endpoints as tools in an
LLM agentic loop (the OmniRouter pattern — each modality exposed as an
OpenAI-compatible tool). The LLM decides which tool to call; this
script executes the tool against Lemonade's API and feeds the result
back.

Prerequisites:
    pip install openai

Running the Lemonade server with the models referenced below already
downloaded is easiest — install LMX-Omni-5.5B-Lite from the desktop app
(Model Manager > Lemonade > LMX-Omni-5.5B-Lite > Download) and
you'll have everything in one click. Otherwise, pull the models below
individually via `lemonade pull <name>`.

Usage:
    python examples/lemonade_tools.py "Generate a 512x512 image of a sunset"
    python examples/lemonade_tools.py "Generate a 16:9 cyberpunk street with seed 1234 and 20 steps"
    python examples/lemonade_tools.py "Say hello world out loud"
"""

import base64
import json
import re
import sys
import urllib.request
from pathlib import Path
from typing import Any

from openai import OpenAI

# Print non-ASCII characters (emoji) without choking on Windows cp1252
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

LEMONADE_URL = "http://localhost:13305/v1"

# Edit these to match models you have installed. Defaults are small so
# they fit on most hardware (and match LMX-Omni-5.5B-Lite).
LLM_MODEL = "Qwen3.5-4B-MTP-GGUF"  # any model with the "tool-calling" label
IMAGE_MODEL = "SD-Turbo"  # any model with the "image" label
TTS_MODEL = "kokoro-v1"  # any model with the "tts" label

DEFAULT_IMAGE_SIZE = "512x512"
MAX_IMAGES_PER_CALL = 4
MAX_IMAGE_DIMENSION = 2048

ASPECT_RATIO_TO_SIZE = {
    "1:1": "512x512",
    "16:9": "1024x576",
    "9:16": "576x1024",
    "4:3": "768x576",
    "3:4": "576x768",
    "3:2": "768x512",
    "2:3": "512x768",
}

ORIENTATION_TO_SIZE = {
    "square": "512x512",
    "landscape": "768x512",
    "wide": "768x512",
    "horizontal": "768x512",
    "portrait": "512x768",
    "vertical": "512x768",
    "tall": "512x768",
}


def image_generation_properties() -> dict[str, Any]:
    """Tool schema shared by generate_image and downstream examples."""
    return {
        "prompt": {
            "type": "string",
            "description": (
                "A detailed description of the image to generate. Do not put "
                "size-only instructions here when a size/width/height argument can be used."
            ),
        },
        "size": {
            "type": "string",
            "description": (
                "Output image size as WIDTHxHEIGHT pixels. Always set this for image "
                "generation/editing. Use exact user dimensions when provided. Default to "
                "512x512 when no size/orientation is specified. Examples: 512x512, "
                "768x512, 512x768, 1024x576, 576x1024."
            ),
        },
        "width": {
            "type": "integer",
            "description": "Optional output width in pixels. Use with height when dimensions are provided separately.",
            "minimum": 64,
        },
        "height": {
            "type": "integer",
            "description": "Optional output height in pixels. Use with width when dimensions are provided separately.",
            "minimum": 64,
        },
        "aspect_ratio": {
            "type": "string",
            "description": "Optional aspect ratio, for example 1:1, 16:9, 9:16, 4:3, or 3:4.",
        },
        "orientation": {
            "type": "string",
            "enum": ["square", "landscape", "portrait"],
            "description": "Optional orientation when no exact dimensions are provided.",
        },
        "steps": {
            "type": "integer",
            "description": "Optional sampling/denoising step count.",
            "minimum": 1,
            "maximum": 100,
        },
        "cfg_scale": {
            "type": "number",
            "description": "Optional text guidance scale. Higher values follow the prompt more strongly.",
            "minimum": 0,
        },
        "seed": {
            "type": "integer",
            "description": "Optional random seed for reproducible generation.",
        },
        "sample_method": {
            "type": "string",
            "description": "Optional sampler name/method. Use only when explicitly requested.",
        },
        "flow_shift": {
            "type": "number",
            "description": "Optional flow shift value for models/backends that support it.",
            "minimum": 0,
        },
        "n": {
            "type": "integer",
            "description": "Optional number of images to generate. Defaults to 1.",
            "minimum": 1,
            "maximum": MAX_IMAGES_PER_CALL,
        },
    }


# Tool definitions — same format src/app/src/renderer/utils/toolDefinitions.json uses
TOOLS = [
    {
        "type": "function",
        "function": {
            "name": "generate_image",
            "description": (
                "Generate a new image from a text description. Always pass size. "
                "Pass through explicitly requested image options such as width, height, "
                "aspect_ratio, orientation, steps, cfg_scale, seed, sample_method, flow_shift, or n."
            ),
            "parameters": {
                "type": "object",
                "properties": image_generation_properties(),
                "required": ["prompt", "size"],
                "additionalProperties": False,
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
                    "voice": {
                        "type": "string",
                        "description": "Voice to use for speech synthesis",
                        "default": "af_heart",
                    },
                },
                "required": ["input"],
                "additionalProperties": False,
            },
        },
    },
]

SYSTEM_PROMPT = (
    "You are a helpful assistant with access to tools for generating images "
    "and converting text to speech. Use the appropriate tool when the user "
    "asks for an image or audio. When generating images, always pass a size. "
    "If exact dimensions are requested, preserve them as WIDTHxHEIGHT. If only "
    "orientation/aspect ratio is requested, choose: square/1:1 -> 512x512, "
    "landscape/wide -> 768x512, portrait/vertical -> 512x768, 16:9 -> 1024x576, "
    "9:16 -> 576x1024, 4:3 -> 768x576, 3:4 -> 576x768. If no size is requested, "
    "use 512x512. Preserve steps, cfg_scale, seed, sample_method, flow_shift, "
    "and n as tool arguments. After using a tool, briefly describe what you did."
)


def _coerce_int(value: Any) -> int | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, int):
        return value
    if isinstance(value, float) and value.is_integer():
        return int(value)
    if isinstance(value, str):
        try:
            return int(value.strip())
        except ValueError:
            return None
    return None


def _coerce_float(value: Any) -> float | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, str):
        try:
            return float(value.strip())
        except ValueError:
            return None
    return None


def _valid_dimension(value: int) -> bool:
    return 64 <= value <= MAX_IMAGE_DIMENSION


def _format_size(width: int, height: int) -> str | None:
    if _valid_dimension(width) and _valid_dimension(height):
        return f"{width}x{height}"
    return None


def _parse_size_from_text(text: str) -> str | None:
    """Parse explicit dimensions from free text such as '1024x576'."""
    if not text:
        return None

    match = re.search(r"(?<!\d)(\d{2,4})\s*(?:x|×|by)\s*(\d{2,4})(?!\d)", text, re.IGNORECASE)
    if match:
        parsed = _format_size(int(match.group(1)), int(match.group(2)))
        if parsed:
            return parsed

    # Also support phrases such as "width 768 height 512".
    width_match = re.search(r"\bwidth\s*[:=]?\s*(\d{2,4})\b", text, re.IGNORECASE)
    height_match = re.search(r"\bheight\s*[:=]?\s*(\d{2,4})\b", text, re.IGNORECASE)
    if width_match and height_match:
        parsed = _format_size(int(width_match.group(1)), int(height_match.group(1)))
        if parsed:
            return parsed

    return None


def _normalize_aspect_ratio(value: str) -> str:
    compact = value.strip().lower().replace(" ", "")
    compact = compact.replace("/", ":")
    return compact


def _size_from_ratio_or_orientation(args: dict[str, Any]) -> str | None:
    candidates: list[str] = []
    for key in ("aspect_ratio", "orientation", "size", "prompt"):
        value = args.get(key)
        if isinstance(value, str):
            candidates.append(value)
    text = " ".join(candidates).lower()

    ratio_value = args.get("aspect_ratio")
    if isinstance(ratio_value, str):
        ratio = _normalize_aspect_ratio(ratio_value)
        if ratio in ASPECT_RATIO_TO_SIZE:
            return ASPECT_RATIO_TO_SIZE[ratio]

    for ratio, size in ASPECT_RATIO_TO_SIZE.items():
        left, right = ratio.split(":")
        if re.search(rf"(?<!\d){left}\s*[:/]\s*{right}(?!\d)", text):
            return size

    orientation_value = args.get("orientation")
    if isinstance(orientation_value, str):
        orientation = orientation_value.strip().lower()
        if orientation in ORIENTATION_TO_SIZE:
            return ORIENTATION_TO_SIZE[orientation]

    if re.search(r"\b(square|1\s*[:/]\s*1)\b", text):
        return "512x512"
    if re.search(r"\b(portrait|vertical|tall)\b", text):
        return "512x768"
    if re.search(r"\b(landscape|wide|widescreen|horizontal|banner)\b", text):
        return "768x512"

    return None


def resolve_image_size(args: dict[str, Any]) -> str:
    """Return a safe WIDTHxHEIGHT size string for Lemonade's image API.

    Precedence:
    1. args.size
    2. args.width + args.height
    3. explicit dimensions embedded in prompt text
    4. aspect ratio / orientation arguments or words
    5. neutral square default
    """
    raw_size = args.get("size")
    if isinstance(raw_size, str):
        parsed = _parse_size_from_text(raw_size)
        if parsed:
            return parsed

    width = _coerce_int(args.get("width"))
    height = _coerce_int(args.get("height"))
    if width is not None and height is not None:
        parsed = _format_size(width, height)
        if parsed:
            return parsed

    prompt = args.get("prompt")
    if isinstance(prompt, str):
        parsed = _parse_size_from_text(prompt)
        if parsed:
            return parsed

    inferred = _size_from_ratio_or_orientation(args)
    if inferred:
        return inferred

    return DEFAULT_IMAGE_SIZE


def resolve_image_count(args: dict[str, Any]) -> int:
    n = _coerce_int(args.get("n"))
    if n is None:
        return 1
    return max(1, min(MAX_IMAGES_PER_CALL, n))


def build_image_extra_body(args: dict[str, Any]) -> dict[str, Any]:
    """Collect optional Lemonade/sd-cpp image parameters.

    Lemonade's SD backend reads steps, cfg_scale, sample_method, flow_shift,
    and seed from the top-level request and embeds the compatible sd.cpp args
    in the forwarded prompt.
    """
    extra: dict[str, Any] = {}

    steps = _coerce_int(args.get("steps"))
    if steps is not None and steps > 0:
        extra["steps"] = steps

    cfg_scale = _coerce_float(args.get("cfg_scale"))
    if cfg_scale is not None and cfg_scale > 0:
        extra["cfg_scale"] = cfg_scale

    seed = _coerce_int(args.get("seed"))
    if seed is not None:
        extra["seed"] = seed

    sample_method = args.get("sample_method")
    if isinstance(sample_method, str) and sample_method.strip():
        extra["sample_method"] = sample_method.strip()

    flow_shift = _coerce_float(args.get("flow_shift"))
    if flow_shift is not None and flow_shift > 0:
        extra["flow_shift"] = flow_shift

    return extra


def save_generated_images(result: Any, stem: str = "output") -> list[Path]:
    paths: list[Path] = []
    for index, item in enumerate(result.data):
        image_b64 = item.b64_json
        filename = f"{stem}.png" if len(result.data) == 1 else f"{stem}_{index + 1}.png"
        path = Path(filename)
        path.write_bytes(base64.b64decode(image_b64))
        paths.append(path)
    return paths


def execute_tool(client: OpenAI, tool_call: Any) -> str:
    name = tool_call.function.name
    args = json.loads(tool_call.function.arguments or "{}")

    if name == "generate_image":
        size = resolve_image_size(args)
        n = resolve_image_count(args)
        extra_body = build_image_extra_body(args)

        request_args: dict[str, Any] = {
            "model": IMAGE_MODEL,
            "prompt": args.get("prompt", ""),
            "response_format": "b64_json",
            "n": n,
            "size": size,
        }
        if extra_body:
            request_args["extra_body"] = extra_body

        result = client.images.generate(**request_args)
        paths = save_generated_images(result)
        joined_paths = ", ".join(str(path) for path in paths)
        options = {"size": size, "n": n, **extra_body}
        print(f"  -> Image saved to {joined_paths}")
        print(f"  -> Image options: {json.dumps(options, ensure_ascii=False)}")
        return f"Image generated and saved to {joined_paths}. Options used: {options}."

    if name == "text_to_speech":
        audio = client.audio.speech.create(
            model=TTS_MODEL,
            input=args["input"],
            voice=args.get("voice") or "af_heart",
        )
        audio.write_to_file("output.wav")
        print("  -> Audio saved to output.wav")
        return "Audio generated and saved to output.wav."

    return f"Unknown tool: {name}"


def preflight_models() -> None:
    """Hit /v1/models?show_all=true and fail loudly if any hardcoded
    model name isn't present. Without this, the first tool call just
    returns a 404 and it's not obvious what went wrong."""
    try:
        with urllib.request.urlopen(
            f"{LEMONADE_URL}/models?show_all=true", timeout=5
        ) as r:
            models = {m["id"]: m for m in json.load(r).get("data", [])}
    except Exception as e:
        print(f"Can't reach Lemonade at {LEMONADE_URL}: {e}", file=sys.stderr)
        print("Is the server running? (desktop app, or `lemond`)", file=sys.stderr)
        sys.exit(1)

    missing = [
        name for name in (LLM_MODEL, IMAGE_MODEL, TTS_MODEL) if name not in models
    ]
    if missing:
        print(f"Required models not installed: {', '.join(missing)}", file=sys.stderr)
        print(
            "Fix: open the desktop app and download LMX-Omni-5.5B-Lite,",
            file=sys.stderr,
        )
        print(
            "or edit LLM_MODEL / IMAGE_MODEL / TTS_MODEL at the top of", file=sys.stderr
        )
        print("this script to match models you already have.", file=sys.stderr)
        sys.exit(1)


def main() -> None:
    prompt = (
        " ".join(sys.argv[1:])
        if len(sys.argv) > 1
        else "Generate a 512x512 image of a cat in space with seed 1234"
    )
    print(f"User: {prompt}\n")

    preflight_models()

    client = OpenAI(base_url=LEMONADE_URL, api_key="not-needed")

    messages = [
        {"role": "system", "content": SYSTEM_PROMPT},
        {"role": "user", "content": prompt},
    ]

    # Agentic loop (max 3 iterations)
    for _ in range(3):
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

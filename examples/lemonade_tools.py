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
    python examples/lemonade_tools.py "Generate an image of a sunset"
    python examples/lemonade_tools.py "Generate a 2:1 cyberpunk street with seed 1234 and 20 steps"
    python examples/lemonade_tools.py "Say hello world out loud"

For image edits, set IMAGE_MODEL below to an installed model with the
"edit" label, generate an image first, then ask to modify it in the
same prompt, for example: "Generate a robot, then make its eyes blue".
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
IMAGE_MODEL = "SD-Turbo"  # any model with the "image" label; edits require "edit"
TTS_MODEL = "kokoro-v1"  # any model with the "tts" label

DEFAULT_IMAGE_SIZE = "640x320"
MAX_IMAGES_PER_CALL = 4
MAX_IMAGE_DIMENSION = 2048

IMAGE_SIZE_PRESETS = [
    {"size": DEFAULT_IMAGE_SIZE, "ratios": ["2:1"], "hints": ["landscape", "wide", "widescreen", "horizontal", "banner"]},
    {"size": "512x512", "ratios": ["1:1"], "hints": ["square"]},
    {"size": "1024x576", "ratios": ["16:9"]},
    {"size": "576x1024", "ratios": ["9:16"]},
    {"size": "768x576", "ratios": ["4:3"]},
    {"size": "576x768", "ratios": ["3:4"]},
    {"size": "768x512", "ratios": ["3:2"]},
    {"size": "512x768", "ratios": ["2:3"], "hints": ["portrait", "vertical", "tall"]},
]

ASPECT_RATIO_TO_SIZE = {
    ratio: preset["size"]
    for preset in IMAGE_SIZE_PRESETS
    for ratio in preset.get("ratios", [])
}

SIZE_HINT_TO_SIZE = {
    hint: preset["size"]
    for preset in IMAGE_SIZE_PRESETS
    for hint in preset.get("hints", [])
}


def image_tool_properties(prompt_description: str) -> dict[str, Any]:
    """Tool schema shared by generate_image and edit_image.

    The schema intentionally avoids separate aspect_ratio/orientation
    parameters. Natural-language hints stay in the prompt and are resolved by
    the executor, so the planner does not need to spend tokens on aliases such
    as portrait/vertical/tall that all map to the same size.
    """
    return {
        "prompt": {
            "type": "string",
            "description": prompt_description,
        },
        "size": {
            "type": "string",
            "description": (
                "Optional output image size as WIDTHxHEIGHT pixels. Use exact user "
                f"dimensions when provided; otherwise omit to use the {DEFAULT_IMAGE_SIZE} default."
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


def image_tool_definition(name: str, description: str, prompt_description: str) -> dict[str, Any]:
    return {
        "type": "function",
        "function": {
            "name": name,
            "description": description,
            "parameters": {
                "type": "object",
                "properties": image_tool_properties(prompt_description),
                "required": ["prompt"],
                "additionalProperties": False,
            },
        },
    }


def text_to_speech_tool_definition() -> dict[str, Any]:
    return {
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
    }


def model_has_label(models: dict[str, Any], model_name: str, label: str) -> bool:
    return label in (models.get(model_name, {}).get("labels") or [])


def build_tools(models: dict[str, Any]) -> list[dict[str, Any]]:
    tools = []
    if model_has_label(models, IMAGE_MODEL, "image"):
        tools.append(
            image_tool_definition(
                "generate_image",
                "Generate a new image from scratch based on a text description.",
                "A detailed description of the image to generate. Keep orientation/aspect-ratio words in the prompt unless passing an exact size.",
            )
        )
        if model_has_label(models, IMAGE_MODEL, "edit"):
            tools.append(
                image_tool_definition(
                    "edit_image",
                    "Edit or modify the most recently generated image. Use this for changes to an existing image, not for a brand new image.",
                    "A description of the edit to apply. Keep orientation/aspect-ratio words in the prompt unless passing an exact size.",
                )
            )
    if model_has_label(models, TTS_MODEL, "tts"):
        tools.append(text_to_speech_tool_definition())
    return tools


def build_system_prompt(tools: list[dict[str, Any]]) -> str:
    names = {tool["function"]["name"] for tool in tools}
    instructions = [
        "You are a helpful assistant. Use the listed tools when the user's request matches one of them.",
    ]
    if "edit_image" in names:
        instructions.append(
            "When the user wants to change a generated image, call edit_image rather than generate_image."
        )
    if {"generate_image", "edit_image"} & names:
        instructions.append(
            "For images, pass size or width+height only for exact dimensions. For aspect-ratio or orientation requests, pass a concrete size only when obvious; otherwise keep the hint in the prompt and let the executor resolve it. Omit size args to use the default. Preserve steps, cfg_scale, seed, sample_method, flow_shift, and n when requested."
        )
    instructions.append("After using a tool, briefly describe what you did.")
    return "\n".join(instructions)


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

    width_match = re.search(r"\bwidth\s*[:=]?\s*(\d{2,4})\b", text, re.IGNORECASE)
    height_match = re.search(r"\bheight\s*[:=]?\s*(\d{2,4})\b", text, re.IGNORECASE)
    if width_match and height_match:
        parsed = _format_size(int(width_match.group(1)), int(height_match.group(1)))
        if parsed:
            return parsed

    return None


def _normalize_aspect_ratio(value: str) -> str:
    return value.strip().lower().replace(" ", "").replace("/", ":")


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
        if orientation in SIZE_HINT_TO_SIZE:
            return SIZE_HINT_TO_SIZE[orientation]

    for hint, size in SIZE_HINT_TO_SIZE.items():
        if re.search(rf"\b{hint}\b", text):
            return size

    return None


def resolve_image_size(args: dict[str, Any]) -> str:
    """Return a safe WIDTHxHEIGHT size string for Lemonade's image API.

    Precedence:
    1. args.size
    2. args.width + args.height
    3. explicit dimensions embedded in prompt text
    4. aspect ratio / orientation arguments or words
    5. 640x320 2:1 default
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
    """Collect optional Lemonade/sd-cpp image parameters."""
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


def execute_tool(client: OpenAI, tool_call: Any, previous_images: list[Path]) -> str:
    name = tool_call.function.name
    args = json.loads(tool_call.function.arguments or "{}")

    if name in {"generate_image", "edit_image"}:
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

        if name == "edit_image":
            if not previous_images:
                return "No previous image is available to edit. Generate an image first."
            with previous_images[-1].open("rb") as source_image:
                result = client.images.edit(image=source_image, **request_args)
            paths = save_generated_images(result, "edited_output")
            action = "edited"
        else:
            result = client.images.generate(**request_args)
            paths = save_generated_images(result)
            action = "generated"

        previous_images.extend(paths)
        joined_paths = ", ".join(str(path) for path in paths)
        options = {"size": size, "n": n, **extra_body}
        print(f"  -> Image {action} and saved to {joined_paths}")
        print(f"  -> Image options: {json.dumps(options, ensure_ascii=False)}")
        return f"Image {action} and saved to {joined_paths}. Options used: {options}."

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


def preflight_models() -> dict[str, Any]:
    """Hit /v1/models?show_all=true and fail loudly if hardcoded model names are missing."""
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

    return models


def main() -> None:
    prompt = (
        " ".join(sys.argv[1:])
        if len(sys.argv) > 1
        else "Generate an image of a cat in space with seed 1234"
    )
    print(f"User: {prompt}\n")

    models = preflight_models()
    tools = build_tools(models)
    if not tools:
        print("No demo tools are available for the configured models.", file=sys.stderr)
        sys.exit(1)

    client = OpenAI(base_url=LEMONADE_URL, api_key="not-needed")
    previous_images: list[Path] = []

    messages = [
        {"role": "system", "content": build_system_prompt(tools)},
        {"role": "user", "content": prompt},
    ]

    # Agentic loop (max 3 iterations)
    for _ in range(3):
        response = client.chat.completions.create(
            model=LLM_MODEL,
            messages=messages,
            tools=tools,
        )

        message = response.choices[0].message

        if not message.tool_calls:
            print(f"Assistant: {message.content}")
            break

        messages.append(message)

        for tool_call in message.tool_calls:
            print(f"  [Tool] {tool_call.function.name}({tool_call.function.arguments})")
            result = execute_tool(client, tool_call, previous_images)
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

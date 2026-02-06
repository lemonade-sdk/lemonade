"""
This example demonstrates how to use the lemonade server API to generate
images using Stable Diffusion models via the OpenAI Python client.

Prerequisites:
1. Install the OpenAI client: pip install openai
2. Start the lemonade server: lemonade-server
3. The SD-Turbo model will be auto-downloaded on first use

Usage:
    python api_image_generation.py
"""

import base64
from pathlib import Path


def generate_with_openai_client():
    """Generate image using the OpenAI Python client."""
    try:
        from openai import OpenAI
    except ImportError:
        print("OpenAI client not installed. Install with: pip install openai")
        return None

    # Point to local lemonade server
    client = OpenAI(
        base_url="http://localhost:8000/api/v1",
        api_key="not-needed",  # Lemonade doesn't require API key
    )

    print("Generating image with OpenAI client...")
    print("(This may take several minutes with CPU backend)")

    response = client.images.generate(
        model="SD-Turbo",
        prompt="A serene mountain landscape at sunset, digital art",
        size="512x512",
        n=1,
        response_format="b64_json",
        # SD-specific parameters (passed through)
        extra_body={
            "steps": 4,  # SD-Turbo works well with 4 steps
            "cfg_scale": 1.0,  # SD-Turbo uses low CFG
        },
    )

    # Save the image
    if response.data:
        image_data = base64.b64decode(response.data[0].b64_json)
        output_path = Path("generated_image_openai.png")
        output_path.write_bytes(image_data)
        print(f"Image saved to: {output_path.absolute()}")
        return output_path

    return None


if __name__ == "__main__":
    print("=" * 60)
    print("Lemonade Image Generation Example")
    print("=" * 60)
    print()
    print("Make sure the lemonade server is running:")
    print("  lemonade-server")
    print()

    # Generate using OpenAI client
    result = generate_with_openai_client()

    print()
    print("=" * 60)
    print("Done!")
    if result:
        print(f"Generated image saved to: {result}")

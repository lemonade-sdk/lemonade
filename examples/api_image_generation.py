"""
This example demonstrates how to use the lemonade server API to generate
images using Stable Diffusion models.

Prerequisites:
1. Start the lemonade server:
   lemonade-server --save-images  # Optional: save generated images to disk

2. Pull the SD-Turbo model (if not already downloaded):
   The model will be auto-downloaded on first use, or you can manually register it.

Usage:
    python api_image_generation.py

The example shows two approaches:
1. Using the OpenAI Python client (recommended)
2. Using direct HTTP requests
"""

import base64
import os
from pathlib import Path

# Approach 1: Using OpenAI client (recommended)
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
        api_key="not-needed"  # Lemonade doesn't require API key
    )

    print("Generating image with OpenAI client...")
    print("(This may take several minutes with CPU backend)")

    response = client.images.generate(
        model="user.SD-Turbo",
        prompt="A serene mountain landscape at sunset, digital art",
        size="512x512",
        n=1,
        response_format="b64_json",
        # SD-specific parameters (passed through)
        extra_body={
            "steps": 4,      # SD-Turbo works well with 4 steps
            "cfg_scale": 1.0  # SD-Turbo uses low CFG
        }
    )

    # Save the image
    if response.data:
        image_data = base64.b64decode(response.data[0].b64_json)
        output_path = Path("generated_image_openai.png")
        output_path.write_bytes(image_data)
        print(f"Image saved to: {output_path.absolute()}")
        return output_path

    return None


# Approach 2: Using direct HTTP requests
def generate_with_requests():
    """Generate image using direct HTTP requests."""
    import requests

    print("Generating image with direct HTTP requests...")
    print("(This may take several minutes with CPU backend)")

    response = requests.post(
        "http://localhost:8000/api/v1/images/generations",
        json={
            "model": "user.SD-Turbo",
            "prompt": "A cute robot holding a flower, cartoon style",
            "size": "512x512",
            "steps": 4,
            "cfg_scale": 1.0,
            "seed": 42,  # For reproducibility
            "response_format": "b64_json"
        },
        timeout=600  # 10 minute timeout for CPU inference
    )

    if response.status_code == 200:
        result = response.json()
        if result.get("data"):
            image_data = base64.b64decode(result["data"][0]["b64_json"])
            output_path = Path("generated_image_requests.png")
            output_path.write_bytes(image_data)
            print(f"Image saved to: {output_path.absolute()}")
            return output_path
    else:
        print(f"Error: {response.status_code} - {response.text}")

    return None


if __name__ == "__main__":
    print("=" * 60)
    print("Lemonade Image Generation Example")
    print("=" * 60)
    print()
    print("Make sure the lemonade server is running:")
    print("  lemonade-server")
    print()

    # Try OpenAI client first
    print("-" * 40)
    print("Method 1: OpenAI Client")
    print("-" * 40)
    result1 = generate_with_openai_client()

    print()

    # Then try direct requests
    print("-" * 40)
    print("Method 2: Direct HTTP Requests")
    print("-" * 40)
    result2 = generate_with_requests()

    print()
    print("=" * 60)
    print("Done!")
    if result1 or result2:
        print("Generated images saved to current directory.")

"""
This example demonstrates how to use the lemonade server API to edit
images using Stable Diffusion models via the OpenAI Python client.

Prerequisites:
1. Install the OpenAI client: pip install openai pillow
2. Start the lemonade server with SD backend: lemonade-server --sdcpp rocm  (or --sdcpp cpu)
3. An image editing model will be auto-downloaded on first use
4. You need a source image to edit (example creates a simple one)

Usage:
    python api_image_edits.py
    python api_image_edits.py --backend rocm
    python api_image_edits.py --backend cpu
    python api_image_edits.py --image path/to/your/image.png
"""

import base64
import argparse
from pathlib import Path
from io import BytesIO


def create_sample_image():
    """Create a simple sample image for testing if none provided."""
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print("Pillow not installed. Install with: pip install pillow")
        return None

    # Create a 512x512 white image with a simple shape
    img = Image.new("RGB", (512, 512), color="white")
    draw = ImageDraw.Draw(img)

    # Draw a simple landscape: green ground, blue sky, yellow sun
    draw.rectangle([(0, 256), (512, 512)], fill="green")  # Ground
    draw.rectangle([(0, 0), (512, 256)], fill="lightblue")  # Sky
    draw.ellipse([(400, 50), (480, 130)], fill="yellow")  # Sun

    output_path = Path("sample_image.png")
    img.save(output_path)
    print(f"Created sample image: {output_path.absolute()}")
    return output_path


def edit_image_with_openai_client(image_path, backend="cpu"):
    """Edit an image using the OpenAI Python client."""
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

    print(f"Editing image with OpenAI client (backend: {backend})...")

    if backend == "cpu":
        print("(This may take several minutes with CPU backend)")

    # Read the image file
    with open(image_path, "rb") as image_file:
        response = client.images.edit(
            model="Flux-2-Klein-4B",  # or another editing model
            image=image_file,
            prompt="Add a red barn and mountains in the background, photorealistic",
            size="512x512",
            n=1,
        )

    # Save the edited image
    if response.data:
        image_data = base64.b64decode(response.data[0].b64_json)
        output_path = Path("edited_image_openai.png")
        output_path.write_bytes(image_data)
        print(f"Edited image saved to: {output_path.absolute()}")
        return output_path

    return None


def edit_image_with_requests(image_path, backend="cpu"):
    """Edit an image using the requests library with multipart form data."""
    try:
        import requests
    except ImportError:
        print("Requests not installed. Install with: pip install requests")
        return None

    print(f"Editing image with requests library (backend: {backend})...")

    if backend == "cpu":
        print("(This may take several minutes with CPU backend)")

    # Prepare the multipart form data
    with open(image_path, "rb") as image_file:
        files = {
            "image": ("image.png", image_file, "image/png"),
        }
        data = {
            "model": "SD-Turbo",
            "prompt": "Add a red barn and mountains in the background, photorealistic",
            "size": "512x512",
            "n": "1",
            "response_format": "b64_json",
        }

        response = requests.post(
            "http://localhost:8000/api/v1/images/edits",
            files=files,
            data=data,
            timeout=600,  # 10 minutes for image generation
        )

    if response.status_code == 200:
        result = response.json()
        if "data" in result and len(result["data"]) > 0:
            image_data = base64.b64decode(result["data"][0]["b64_json"])
            output_path = Path("edited_image_requests.png")
            output_path.write_bytes(image_data)
            print(f"Edited image saved to: {output_path.absolute()}")
            return output_path
        else:
            print(f"Unexpected response format: {result}")
    else:
        print(f"Error: {response.status_code}")
        print(response.text)

    return None


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Edit images using Lemonade server with Stable Diffusion"
    )
    parser.add_argument(
        "--backend",
        type=str,
        choices=["cpu", "rocm"],
        default="cpu",
        help="Backend to use for image editing (default: cpu). Use 'rocm' for AMD GPU acceleration.",
    )
    parser.add_argument(
        "--image",
        type=str,
        help="Path to the image to edit. If not provided, a sample image will be created.",
    )
    parser.add_argument(
        "--method",
        type=str,
        choices=["openai", "requests", "both"],
        default="both",
        help="Which method to use for API calls (default: both)",
    )
    args = parser.parse_args()

    print("=" * 60)
    print("Lemonade Image Editing Example")
    print("=" * 60)
    print()
    print("Make sure the lemonade server is running with SD backend:")
    print("  lemonade-server --sdcpp rocm")
    print()

    # Get or create an image
    if args.image:
        image_path = Path(args.image)
        if not image_path.exists():
            print(f"Error: Image file not found: {image_path}")
            exit(1)
    else:
        image_path = create_sample_image()
        if not image_path:
            exit(1)

    print()

    # Try editing with different methods
    results = []

    if args.method in ["openai", "both"]:
        print("--- Using OpenAI Client ---")
        result = edit_image_with_openai_client(image_path, args.backend)
        if result:
            results.append(result)
        print()

    if args.method in ["requests", "both"]:
        print("--- Using Requests Library ---")
        result = edit_image_with_requests(image_path, args.backend)
        if result:
            results.append(result)
        print()

    print("=" * 60)
    print("Done!")
    if results:
        print("Generated images:")
        for result in results:
            print(f"  - {result}")
    print()
    print("Note: The actual editing capabilities depend on the sd-cpp backend")
    print("and the loaded model. Some models may not support image editing yet.")

"""
This example demonstrates how to use the lemonade server API to create
variations of images using Stable Diffusion models via the OpenAI Python client.

Prerequisites:
1. Install the OpenAI client: pip install openai pillow
2. Start the lemonade server with SD backend: lemonade-server --sdcpp rocm  (or --sdcpp cpu)
3. An image model will be auto-downloaded on first use
4. You need a source image (example creates a simple one)

Usage:
    python api_image_variations.py
    python api_image_variations.py --backend rocm
    python api_image_variations.py --backend cpu
    python api_image_variations.py --image path/to/your/image.png
    python api_image_variations.py --num-variations 3
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

    # Create a 512x512 image with a simple pattern
    img = Image.new("RGB", (512, 512), color="white")
    draw = ImageDraw.Draw(img)

    # Draw a simple scene
    draw.rectangle([(0, 256), (512, 512)], fill="lightgreen")  # Ground
    draw.rectangle([(0, 0), (512, 256)], fill="skyblue")  # Sky
    draw.ellipse([(350, 50), (450, 150)], fill="gold")  # Sun
    draw.rectangle([(200, 180), (300, 300)], fill="brown")  # Tree trunk
    draw.ellipse([(150, 80), (350, 220)], fill="darkgreen")  # Tree foliage

    output_path = Path("sample_image_variations.png")
    img.save(output_path)
    print(f"Created sample image: {output_path.absolute()}")
    return output_path


def create_variations_with_openai_client(image_path, num_variations=1, backend="cpu"):
    """Create image variations using the OpenAI Python client."""
    try:
        from openai import OpenAI
    except ImportError:
        print("OpenAI client not installed. Install with: pip install openai")
        return []

    # Point to local lemonade server
    client = OpenAI(
        base_url="http://localhost:8000/api/v1",
        api_key="not-needed",  # Lemonade doesn't require API key
    )

    print(
        f"Creating {num_variations} variation(s) with OpenAI client (backend: {backend})..."
    )

    if backend == "cpu":
        print("(This may take several minutes with CPU backend)")

    results = []

    # Read the image file
    with open(image_path, "rb") as image_file:
        response = client.images.create_variation(
            model="Flux-2-Klein-4B",  # or another model
            image=image_file,
            size="512x512",
            n=num_variations,
        )

    # Save the variations
    if response.data:
        for i, image_obj in enumerate(response.data):
            image_data = base64.b64decode(image_obj.b64_json)
            output_path = Path(f"variation_openai_{i+1}.png")
            output_path.write_bytes(image_data)
            print(f"Variation {i+1} saved to: {output_path.absolute()}")
            results.append(output_path)

    return results


def create_variations_with_requests(image_path, num_variations=1, backend="cpu"):
    """Create image variations using the requests library with multipart form data."""
    try:
        import requests
    except ImportError:
        print("Requests not installed. Install with: pip install requests")
        return []

    print(
        f"Creating {num_variations} variation(s) with requests library (backend: {backend})..."
    )

    if backend == "cpu":
        print("(This may take several minutes with CPU backend)")

    results = []

    # Prepare the multipart form data
    with open(image_path, "rb") as image_file:
        files = {
            "image": ("image.png", image_file, "image/png"),
        }
        data = {
            "model": "SD-Turbo",
            "size": "512x512",
            "n": str(num_variations),
        }

        response = requests.post(
            "http://localhost:8000/api/v1/images/variations",
            files=files,
            data=data,
            timeout=600,  # 10 minutes for image generation
        )

    if response.status_code == 200:
        result = response.json()
        if "data" in result and len(result["data"]) > 0:
            for i, image_obj in enumerate(result["data"]):
                image_data = base64.b64decode(image_obj["b64_json"])
                output_path = Path(f"variation_requests_{i+1}.png")
                output_path.write_bytes(image_data)
                print(f"Variation {i+1} saved to: {output_path.absolute()}")
                results.append(output_path)
        else:
            print(f"Unexpected response format: {result}")
    else:
        print(f"Error: {response.status_code}")
        print(response.text)

    return results


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Create image variations using Lemonade server with Stable Diffusion"
    )
    parser.add_argument(
        "--backend",
        type=str,
        choices=["cpu", "rocm"],
        default="cpu",
        help="Backend to use for image variations (default: cpu). Use 'rocm' for AMD GPU acceleration.",
    )
    parser.add_argument(
        "--image",
        type=str,
        help="Path to the source image. If not provided, a sample image will be created.",
    )
    parser.add_argument(
        "--num-variations",
        type=int,
        default=1,
        help="Number of variations to generate (default: 1)",
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
    print("Lemonade Image Variations Example")
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

    # Try creating variations with different methods
    all_results = []

    if args.method in ["openai", "both"]:
        print("--- Using OpenAI Client ---")
        results = create_variations_with_openai_client(
            image_path, args.num_variations, args.backend
        )
        all_results.extend(results)
        print()

    if args.method in ["requests", "both"]:
        print("--- Using Requests Library ---")
        results = create_variations_with_requests(
            image_path, args.num_variations, args.backend
        )
        all_results.extend(results)
        print()

    print("=" * 60)
    print("Done!")
    if all_results:
        print(f"Generated {len(all_results)} variation(s):")
        for result in all_results:
            print(f"  - {result}")
    print()
    print("Note: The actual variation capabilities depend on the sd-cpp backend")
    print("and the loaded model. Some models may not support image variations yet.")

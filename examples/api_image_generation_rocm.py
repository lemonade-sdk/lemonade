#!/usr/bin/env python3
"""
ROCm GPU Image Generation Example

Demonstrates AMD GPU-accelerated image generation with stable-diffusion.cpp.

Prerequisites:
    pip install openai requests
    lemonade-server serve --sdcpp rocm  # Start server with ROCm backend

Usage:
    python api_image_gen_rocm.py
"""

import base64
import time
import requests
from openai import OpenAI

# Connect to local lemonade server
BASE_URL = "http://localhost:8000/api/v1"
client = OpenAI(base_url=BASE_URL, api_key="not-needed")

# Models to test (name, steps, cfg_scale)
MODELS = [
    ("SD-Turbo", 4, 1.0),
    ("SD-1.5", 20, 7.0),
    ("SDXL-Turbo", 4, 1.0),
    ("SDXL-Base-1.0", 20, 7.0),
]

prompt = "A majestic dragon breathing fire over a medieval castle"

print("Testing ROCm-accelerated image generation\n")

for model, steps, cfg_scale in MODELS:
    print(f"Generating with {model}...")
    
    # Load model with ROCm backend
    requests.post(
        f"{BASE_URL}/load",
        json={"model_name": model, "sd-cpp_backend": "rocm"},
        timeout=300,
    )
    
    # Generate image
    start = time.time()
    response = client.images.generate(
        model=model,
        prompt=prompt,
        size="512x512",
        n=1,
        response_format="b64_json",
        extra_body={"steps": steps, "cfg_scale": cfg_scale},
    )
    elapsed = time.time() - start
    
    # Save image
    image_data = base64.b64decode(response.data[0].b64_json)
    filename = f"{model.lower().replace('-', '_')}_rocm.png"
    with open(filename, "wb") as f:
        f.write(image_data)
    
    print(f"  ✓ Generated in {elapsed:.2f}s → {filename}\n")

print("Done!")

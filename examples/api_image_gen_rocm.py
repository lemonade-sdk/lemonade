#!/usr/bin/env python3
"""
ROCm GPU Image Generation Example

This example demonstrates using the ROCm backend for AMD GPU-accelerated
image generation with stable-diffusion.cpp via the lemonade-server API.

Requirements:
- AMD GPU with ROCm support (e.g., Radeon 8060S)
- lemonade-server running with ROCm backend:
  $env:LEMONADE_SDCPP = "rocm"
  lemonade-server serve
  
  Or via CLI flag:
  lemonade-server serve --sdcpp rocm

Model:
- This example uses 'SD-Turbo' which is optimized for fast generation
- SD-Turbo requires only 1-4 steps vs 20-50 for standard models
"""

import os
import sys
import requests
import base64
import time
from pathlib import Path

# Add parent directory to path to import lemonade API helpers if available
sys.path.insert(0, str(Path(__file__).parent.parent / "src"))

# Configuration
API_BASE = os.getenv("LEMONADE_API_BASE", "http://localhost:8000/api/v1")
MODEL = "SD-Turbo"  # Fast 1-step diffusion model

def check_server():
    """Check if lemonade-server is running and responsive"""
    try:
        response = requests.get(f"{API_BASE}/health", timeout=2)
        if response.status_code == 200:
            health = response.json()
            print(f"✓ Server is healthy: {health.get('status')}")
            return True
        else:
            print(f"✗ Server returned status {response.status_code}")
            return False
    except requests.exceptions.ConnectionError:
        print(f"✗ Cannot connect to server at {API_BASE}")
        print("  Start server with: lemonade-server serve --sdcpp rocm")
        return False
    except Exception as e:
        print(f"✗ Health check failed: {e}")
        return False

def load_model(model_name):
    """Load the specified SD model"""
    print(f"\nLoading model: {model_name}")
    try:
        response = requests.post(
            f"{API_BASE}/load",
            json={"model_name": model_name},
            timeout=300  # Model download can take time
        )
        
        if response.status_code == 200:
            result = response.json()
            print(f"✓ Model loaded: {result.get('message', 'Success')}")
            return True
        else:
            print(f"✗ Failed to load model: {response.text}")
            return False
            
    except Exception as e:
        print(f"✗ Error loading model: {e}")
        return False

def generate_image(prompt, output_path="generated_rocm.png", steps=1, cfg_scale=1.0):
    """
    Generate an image using the ROCm-accelerated backend
    
    Args:
        prompt: Text description of the image to generate
        output_path: Where to save the generated image
        steps: Number of diffusion steps (1-4 for SD-Turbo)
        cfg_scale: Guidance scale (1.0 recommended for SD-Turbo)
    """
    print(f"\nGenerating image with ROCm GPU backend...")
    print(f"Prompt: {prompt}")
    print(f"Steps: {steps}, CFG Scale: {cfg_scale}")
    
    start_time = time.time()
    
    try:
        response = requests.post(
            f"{API_BASE}/images/generations",
            json={
                "model": MODEL,
                "prompt": prompt,
                "n": 1,
                "size": "512x512",
                "response_format": "b64_json",
                # Model-specific parameters
                "steps": steps,
                "cfg_scale": cfg_scale
            },
            timeout=60
        )
        
        elapsed = time.time() - start_time
        
        if response.status_code == 200:
            result = response.json()
            
            # Extract base64 image data
            if "data" in result and len(result["data"]) > 0:
                image_data = result["data"][0]
                b64_data = image_data.get("b64_json", "")
                
                if b64_data:
                    # Decode and save image
                    image_bytes = base64.b64decode(b64_data)
                    with open(output_path, "wb") as f:
                        f.write(image_bytes)
                    
                    file_size = len(image_bytes) / 1024  # KB
                    print(f"✓ Image generated in {elapsed:.2f}s")
                    print(f"  Saved to: {output_path} ({file_size:.1f} KB)")
                    return True
                else:
                    print("✗ No image data in response")
                    return False
            else:
                print("✗ Unexpected response format")
                print(response.text)
                return False
                
        else:
            print(f"✗ Generation failed (HTTP {response.status_code})")
            print(response.text)
            return False
            
    except Exception as e:
        print(f"✗ Error during generation: {e}")
        return False

def main():
    """Main example workflow"""
    print("=" * 70)
    print("ROCm GPU Image Generation Example")
    print("Using lemonade-server with AMD GPU acceleration")
    print("=" * 70)
    
    # Step 1: Check server
    if not check_server():
        return 1
    
    # Step 2: Load SD-Turbo model
    if not load_model(MODEL):
        return 1
    
    # Step 3: Generate test images
    test_prompts = [
        ("A majestic dragon breathing fire", "dragon_rocm.png", 4, 1.0),
        ("A peaceful zen garden with cherry blossoms", "garden_rocm.png", 4, 1.0),
        ("A futuristic city at night with neon lights", "city_rocm.png", 4, 1.0)
    ]
    
    success_count = 0
    for prompt, filename, steps, cfg in test_prompts:
        if generate_image(prompt, filename, steps, cfg):
            success_count += 1
        print()  # Blank line between generations
    
    # Summary
    print("=" * 70)
    print(f"Generated {success_count}/{len(test_prompts)} images successfully")
    print("=" * 70)
    
    return 0 if success_count > 0 else 1

if __name__ == "__main__":
    sys.exit(main())

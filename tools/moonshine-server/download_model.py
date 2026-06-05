#!/usr/bin/env python3
"""
Download a Moonshine model via moonshine_voice.download.

Usage:
    python3 download_model.py --language en --arch 5

Returns the resolved cache path on stdout, exits 0 on success.
"""

import argparse
import sys
import os


def main():
    parser = argparse.ArgumentParser(description="Download a Moonshine model")
    parser.add_argument("--language", default="en", help="Model language (default: en)")
    parser.add_argument("--arch", type=int, required=True, help="Model architecture integer (e.g., 5=MEDIUM_STREAMING)")
    args = parser.parse_args()

    # vendored moonshine_voice fallback: look relative to this script
    script_dir = os.path.dirname(os.path.abspath(__file__))
    vendored = os.path.join(script_dir, "..", "..", "third_party", "moonshine", "python")
    if os.path.isdir(vendored):
        sys.path.insert(0, vendored)

    try:
        from moonshine_voice.download import get_model_for_language
        from moonshine_voice.moonshine_api import ModelArch
    except ImportError as e:
        print(f"ERROR: moonshine_voice not found: {e}", file=sys.stderr)
        sys.exit(1)

    try:
        model_path, model_arch = get_model_for_language(args.language, ModelArch(args.arch))
        print(model_path, end="")
        sys.exit(0)
    except Exception as e:
        print(f"ERROR: Failed to download model: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()

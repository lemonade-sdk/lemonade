"""Command-line interface for Lemonade Lean."""

import argparse
import logging
import sys


def main():
    """Main CLI entry point."""
    parser = argparse.ArgumentParser(
        description="Lemonade Lean - Minimal LLM server with llama.cpp Vulkan backend"
    )

    subparsers = parser.add_subparsers(dest="command", help="Available commands")

    # Serve command
    serve_parser = subparsers.add_parser("serve", help="Start the server")
    serve_parser.add_argument(
        "--model",
        required=True,
        help="Path to GGUF model file, HuggingFace repo ID (e.g., unsloth/Qwen3-0.6B-GGUF), "
        "or repo:file format (e.g., unsloth/Qwen3-0.6B-GGUF:Qwen3-0.6B-Q4_0.gguf)",
    )
    serve_parser.add_argument(
        "--port", type=int, default=8000, help="Port to serve on (default: 8000)"
    )
    serve_parser.add_argument(
        "--host", default="localhost", help="Host to bind to (default: localhost)"
    )
    serve_parser.add_argument(
        "--ctx-size", type=int, default=4096, help="Context size (default: 4096)"
    )
    serve_parser.add_argument(
        "--log-level",
        choices=["debug", "info", "warning", "error"],
        default="info",
        help="Log level (default: info)",
    )

    # List models command
    list_parser = subparsers.add_parser(
        "list", help="List GGUF models in HuggingFace cache"
    )
    list_parser.add_argument(
        "--cache-dir",
        help="Override HuggingFace cache directory location",
        default=None,
    )

    args = parser.parse_args()

    # Setup logging
    log_level = getattr(
        logging, args.log_level.upper() if hasattr(args, "log_level") else "INFO"
    )
    logging.basicConfig(level=log_level, format="%(levelname)s: %(message)s")

    if args.command == "serve":
        from lemonade_lean.server import LemonadeServer

        logging.info("Starting Lemonade Lean Server...")
        logging.info(f"Model: {args.model}")
        logging.info(f"Server: http://{args.host}:{args.port}")

        server = LemonadeServer(
            model_path=args.model,
            port=args.port,
            host=args.host,
            ctx_size=args.ctx_size,
        )

        try:
            server.run()
        except KeyboardInterrupt:
            logging.info("Server stopped by user")
            sys.exit(0)

    elif args.command == "list":
        from lemonade_lean.hf_cache import (
            find_gguf_models,
            get_hf_cache_dir,
            get_model_snapshot_path,
        )
        import os

        # Override cache dir if specified
        if args.cache_dir:
            os.environ["HF_HUB_CACHE"] = args.cache_dir

        cache_dir = get_hf_cache_dir()
        print(f"Scanning HuggingFace cache: {cache_dir}\n")

        if not cache_dir.exists():
            print(f"Cache directory not found: {cache_dir}")
            print("No models have been downloaded yet.")
            sys.exit(0)

        gguf_models = find_gguf_models()

        if not gguf_models:
            print("No GGUF models found in cache.")
            print("\nTo download a GGUF model:")
            print("  1. Visit https://huggingface.co/")
            print("  2. Search for GGUF models")
            print("  3. Download using 'huggingface-cli' or manually")
            sys.exit(0)

        print(f"Found {len(gguf_models)} GGUF model(s):\n")

        for repo_id, gguf_file in gguf_models:
            snapshot_path = get_model_snapshot_path(repo_id)
            full_path = snapshot_path / gguf_file if snapshot_path else None

            print(f"Repository: {repo_id}")
            print(f"  File: {gguf_file}")
            if full_path:
                print(f"  Path: {full_path}")
            print()

    else:
        parser.print_help()
        sys.exit(1)


if __name__ == "__main__":
    main()

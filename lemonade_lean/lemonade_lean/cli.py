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
    serve_parser.add_argument("--model", required=True, help="Path to GGUF model file")
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
    else:
        parser.print_help()
        sys.exit(1)


if __name__ == "__main__":
    main()

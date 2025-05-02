import argparse
import sys
import os
from typing import Tuple
import psutil
from typing import List


class PullError(Exception):
    """
    The pull command has failed to install an LLM
    """


def serve(port: int):
    """
    Execute the serve command
    """

    # Check if Lemonade Server is already running
    _, running_port = get_server_info()
    if running_port is not None:
        print(
            (
                f"Lemonade Server is already running on port {running_port}\n"
                "Please stop the existing server before starting a new instance."
            ),
        )
        sys.exit(1)

    # Otherwise, start the server
    print("Starting Lemonade Server...")
    from lemonade.tools.serve import Server, DEFAULT_PORT

    server = Server()
    port = port if port is not None else DEFAULT_PORT
    server.run(port=port)


def stop():
    """
    Stop the Lemonade Server
    """

    # Check if Lemonade Server is running
    running_pid, running_port = get_server_info()
    if running_port is None:
        print(f"Lemonade Server is not running\n")
        return

    # Stop the server
    try:
        process = psutil.Process(running_pid)
        process.terminate()
        process.wait(timeout=10)
    except psutil.NoSuchProcess:
        # Process already terminated
        pass
    except psutil.TimeoutExpired:
        print("Timed out waiting for Lemonade Server to stop.")
        sys.exit(1)
    except Exception as e:  # pylint: disable=broad-exception-caught
        print(f"Error stopping Lemonade Server: {e}")
        sys.exit(1)
    print("Lemonade Server stopped successfully.")


def pull(model_names: List[str]):
    """
    Install an LLM based on its Lemonade Server model name

    If Lemonade Server is running, use the pull endpoint to download the model
    so that the Lemonade Server instance is aware of the pull.

    Otherwise, use ModelManager to install the model.
    """

    server_running, port = status(verbose=False)

    if server_running:
        import requests

        base_url = f"http://localhost:{port}/api/v0"

        for model_name in model_names:
            # Install the model
            pull_response = requests.post(
                f"{base_url}/pull", json={"model_name": model_name}
            )

            if pull_response.status_code != 200:
                raise PullError(
                    f"Failed to install {model_name}. Check the "
                    "Lemonade Server log for more information. A list of supported models "
                    "is provided at "
                    "https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/server_models.md"
                )
    else:
        from lemonade_server.model_manager import ModelManager

        ModelManager().download_models(model_names)


def version():
    """
    Print the version number
    """
    from turnkeyml import __version__ as version_number

    print(f"Lemonade Server version is {version_number}")


def status(verbose: bool = True) -> Tuple[bool, int]:
    """
    Print the status of the server

    Returns a tuple of:
    1. Whether the server is running
    2. What port the server is running on (None if server is not running)
    """
    _, port = get_server_info()
    if port is None:
        if verbose:
            print("Server is not running")
        return False, None
    else:
        if verbose:
            print(f"Server is running on port {port}")
        return True, port


def is_lemonade_server(pid):
    """
    Check wether or not a given PID corresponds to a Lemonade server
    """
    try:
        process = psutil.Process(pid)
        while True:
            if process.name() in [  # Windows
                "lemonade-server-dev.exe",
                "lemonade-server.exe",
                "lemonade.exe",
            ] or process.name() in [  # Linux
                "lemonade-server-dev",
                "lemonade-server",
                "lemonade",
            ]:
                return True
            if not process.parent():
                return False
            process = process.parent()
    except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
        return False
    return False


def get_server_info() -> Tuple[int | None, int | None]:
    """
    Returns a tuple of:
    1. Lemonade Server's PID
    2. The port that Lemonade Server is running on
    """
    # Go over all python processes that have a port open
    for process in psutil.process_iter(["pid", "name"]):
        try:
            connections = process.net_connections()
            for conn in connections:
                if conn.status == "LISTEN":
                    if is_lemonade_server(process.info["pid"]):
                        return process.info["pid"], conn.laddr.port
        except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
            continue

    return None, None


def main():
    parser = argparse.ArgumentParser(
        description="Serve LLMs on CPU, GPU, and NPU.",
        usage=argparse.SUPPRESS,
    )

    # Add version flag
    parser.add_argument(
        "-v", "--version", action="store_true", help="Show version number"
    )

    # Create subparsers for commands
    subparsers = parser.add_subparsers(
        title="Available Commands", dest="command", metavar=""
    )

    # Serve command
    serve_parser = subparsers.add_parser("serve", help="Start server")
    serve_parser.add_argument("--port", type=int, help="Port number to serve on")

    # Status command
    status_parser = subparsers.add_parser("status", help="Check if server is running")

    # Stop command
    stop_parser = subparsers.add_parser("stop", help="Stop the server")

    # Pull command
    pull_parser = subparsers.add_parser(
        "pull",
        help="Install an LLM",
        epilog=(
            "More information: "
            "https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/server_models.md"
        ),
    )
    pull_parser.add_argument(
        "model",
        help="Lemonade Server model name",
        nargs="+",
    )

    args = parser.parse_args()

    if args.version:
        version()
    elif args.command == "serve":
        serve(args.port)
    elif args.command == "status":
        status()
    elif args.command == "pull":
        pull(args.model)
    elif args.command == "stop":
        stop()
    elif args.command == "help" or not args.command:
        parser.print_help()


if __name__ == "__main__":
    main()

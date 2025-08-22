import argparse
import sys
import os
from typing import Tuple, Optional
import psutil
from typing import List
from lemonade_server.pydantic_models import (
    DEFAULT_PORT,
    DEFAULT_HOST,
    DEFAULT_LOG_LEVEL,
    DEFAULT_LLAMACPP_BACKEND,
    DEFAULT_CTX_SIZE,
)
from lemonade_server.settings import load_setting


# Error codes for different CLI scenarios
class ExitCodes:
    SUCCESS = 0
    GENERAL_ERROR = 1
    SERVER_ALREADY_RUNNING = 2
    TIMEOUT_STOPPING_SERVER = 3
    ERROR_STOPPING_SERVER = 4


class PullError(Exception):
    """
    The pull command has failed to install an LLM
    """


class DeleteError(Exception):
    """
    The delete command has failed to delete an LLM
    """


class ServerTimeoutError(Exception):
    """
    The server failed to start within the timeout period
    """


class ModelNotAvailableError(Exception):
    """
    The specified model is not available on the server
    """


class ModelLoadError(Exception):
    """
    The model failed to load on the server
    """


def serve(
    port: int = None,
    host: str = None,
    log_level: str = None,
    tray: bool = False,
    use_thread: bool = False,
    llamacpp_backend: str = None,
    ctx_size: int = None,
):
    """
    Execute the serve command
    """

    # Otherwise, start the server
    print("Starting Lemonade Server...")
    from lemonade.tools.server.serve import Server

    port = port if port is not None else DEFAULT_PORT
    host = host if host is not None else DEFAULT_HOST
    log_level = log_level if log_level is not None else DEFAULT_LOG_LEVEL
    llamacpp_backend = (
        llamacpp_backend if llamacpp_backend is not None else DEFAULT_LLAMACPP_BACKEND
    )
    ctx_size = ctx_size if ctx_size is not None else DEFAULT_CTX_SIZE

    # Start the server
    server = Server(
        port=port,
        host=host,
        log_level=log_level,
        ctx_size=ctx_size,
        tray=tray,
        llamacpp_backend=llamacpp_backend,
    )
    if not use_thread:
        server.run()
    else:
        from threading import Thread
        import time

        # Start a background thread to run the server
        server_thread = Thread(
            target=server.run,
            daemon=True,
        )
        server_thread.start()

        # Wait for the server to be ready
        max_wait_time = 30
        wait_interval = 0.5
        waited = 0
        while waited < max_wait_time:
            time.sleep(wait_interval)
            _, running_port = get_server_info()
            if running_port is not None:
                break
            waited += wait_interval

        return port, server_thread


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

        # Get all child processes (including llama-server)
        children = process.children(recursive=True)

        # Terminate the main process first
        process.terminate()

        # Then terminate llama-server child process (known to be stubborn)
        # We avoid killing other child processes, such as the installer
        for child in children:
            if "llama-server" in child.name():
                try:
                    child.terminate()
                except psutil.NoSuchProcess:
                    pass  # Child already terminated

        # Wait for main process to terminate gracefully
        # kill if it doesn't terminate gracefully
        try:
            process.wait(timeout=5)
        except psutil.TimeoutExpired:
            process.kill()

        # Kill llama-server child process if it didn't terminate gracefully
        for child in children:
            if "llama-server" in child.name():
                try:
                    if child.is_running():
                        child.kill()
                except psutil.NoSuchProcess:
                    pass  # Child already terminated
    except psutil.NoSuchProcess:
        # Process already terminated
        pass
    except psutil.TimeoutExpired:
        print("Timed out waiting for Lemonade Server to stop.")
        sys.exit(ExitCodes.TIMEOUT_STOPPING_SERVER)
    except Exception as e:  # pylint: disable=broad-exception-caught
        print(f"Error stopping Lemonade Server: {e}")
        sys.exit(ExitCodes.ERROR_STOPPING_SERVER)
    print("Lemonade Server stopped successfully.")


def pull(
    model_names: List[str],
    checkpoint: Optional[str] = None,
    recipe: Optional[str] = None,
    reasoning: bool = False,
    mmproj: str = "",
):
    """
    Install an LLM based on its Lemonade Server model name

    If Lemonade Server is running, use the pull endpoint to download the model
    so that the Lemonade Server instance is aware of the pull.

    Otherwise, use ModelManager to install the model.
    """

    server_running, port = status(verbose=False)

    if server_running:
        import requests

        base_url = f"http://localhost:{port}/api/v1"

        for model_name in model_names:
            payload = {"model_name": model_name}

            if checkpoint and recipe:
                # Add the parameters for registering a new model
                payload["checkpoint"] = checkpoint
                payload["recipe"] = recipe

                if reasoning:
                    payload["reasoning"] = reasoning
                if mmproj:
                    payload["mmproj"] = mmproj

            # Install the model
            pull_response = requests.post(f"{base_url}/pull", json=payload)

            if pull_response.status_code != 200:
                raise PullError(
                    f"Failed to install {model_name}. Check the "
                    "Lemonade Server log for more information. You can list "
                    "supported models with `lemonade-server list`"
                )
    else:
        from lemonade_server.model_manager import ModelManager

        ModelManager().download_models(
            model_names,
            checkpoint=checkpoint,
            recipe=recipe,
            reasoning=reasoning,
            mmproj=mmproj,
        )


def delete(model_names: List[str]):
    """
    Delete an LLM based on its Lemonade Server model name

    If Lemonade Server is running, use the delete endpoint to delete the model
    so that the Lemonade Server instance is aware of the deletion.

    Otherwise, use ModelManager to delete the model.
    """

    server_running, port = status(verbose=False)

    if server_running:
        import requests

        base_url = f"http://localhost:{port}/api/v1"

        for model_name in model_names:
            # Delete the model
            delete_response = requests.post(
                f"{base_url}/delete", json={"model_name": model_name}
            )

            if delete_response.status_code != 200:
                raise DeleteError(
                    f"Failed to delete {model_name}. Check the "
                    "Lemonade Server log for more information."
                )
    else:
        from lemonade_server.model_manager import ModelManager

        for model_name in model_names:
            ModelManager().delete_model(model_name)


def run(
    model_name: str,
    port: int = None,
    host: str = "localhost",
    log_level: str = None,
    tray: bool = False,
    llamacpp_backend: str = None,
    ctx_size: int = None,
):
    """
    Start the server if not running and open the webapp with the specified model
    """
    import webbrowser
    import time
    import os

    # Start the server if not running
    _, running_port = get_server_info()
    server_previously_running = running_port is not None
    if not server_previously_running:
        port, server_thread = serve(
            port=port,
            host=host,
            log_level=log_level,
            tray=tray,
            use_thread=True,
            llamacpp_backend=llamacpp_backend,
            ctx_size=ctx_size,
        )
    else:
        port = running_port

    # Pull model
    pull([model_name])

    # Load model
    load(model_name, port)

    # Open the webapp with the specified model
    url = f"http://{host}:{port}/?model={model_name}#llm-chat"
    print(f"You can now chat with {model_name} at {url}")

    # Only open browser if not disabled via environment variable
    if not os.environ.get("LEMONADE_DISABLE_BROWSER"):
        webbrowser.open(url)

    # Keep the server running if we started it
    if not server_previously_running:
        while server_thread.is_alive():
            time.sleep(0.5)


def load(model_name: str, port: int):
    """
    Load a model using the endpoint
    """
    import requests

    base_url = f"http://localhost:{port}/api/v1"

    # Load the model
    load_response = requests.post(f"{base_url}/load", json={"model_name": model_name})
    if load_response.status_code != 200:
        raise ModelLoadError(
            f"Failed to load {model_name}. Check the "
            "Lemonade Server log for more information."
        )


def version():
    """
    Print the version number
    """
    from lemonade import __version__ as version_number

    print(f"{version_number}")


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
    Check whether or not a given PID corresponds to a Lemonade server
    """
    try:
        process = psutil.Process(pid)

        while True:
            process_name = process.name()
            if process_name in [  # Windows
                "lemonade-server-dev.exe",
                "lemonade-server.exe",
            ] or process_name in [  # Linux
                "lemonade-server-dev",
                "lemonade-server",
            ]:
                return True
            elif "llama-server" in process_name:
                return False
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

    # Get all network connections and filter for localhost IPv4 listening ports
    try:
        connections = psutil.net_connections(kind="tcp4")

        for conn in connections:
            if conn.status == "LISTEN" and conn.laddr and conn.pid is not None:
                if is_lemonade_server(conn.pid):
                    return conn.pid, conn.laddr.port

    except Exception:
        pass

    return None, None


def list_models():
    """
    List recommended models and their download status
    """
    from tabulate import tabulate
    from lemonade_server.model_manager import ModelManager

    model_manager = ModelManager()

    # Get all supported models and downloaded models
    supported_models = model_manager.supported_models
    downloaded_models = model_manager.downloaded_models

    # Filter to only show recommended models
    recommended_models = {
        model_name: model_info
        for model_name, model_info in supported_models.items()
        if model_info.get("suggested", False)
    }

    # Create table data
    table_data = []
    for model_name, model_info in recommended_models.items():
        downloaded_status = "Yes" if model_name in downloaded_models else "No"

        # Get model labels/type
        labels = model_info.get("labels", [])
        model_type = ", ".join(labels) if labels else "-"

        table_data.append([model_name, downloaded_status, model_type])

    # Sort by model name for consistent display
    # Show downloaded models first
    table_data.sort(key=lambda x: (x[1] == "No", x[0].lower()))

    # Display table
    headers = ["Model Name", "Downloaded", "Details"]
    print(tabulate(table_data, headers=headers, tablefmt="simple"))


def developer_entrypoint():
    """
    Developer entry point that starts the server with debug logging
    Equivalent to running: lemonade-server-dev serve --log-level debug [additional args]

    This function automatically prepends "serve --log-level debug" to any arguments
    passed to the lsdev command.
    """
    # Save original sys.argv
    original_argv = sys.argv.copy()

    try:
        # Take any additional arguments passed to lsdev and append them
        # after "serve --log-level debug"
        additional_args = sys.argv[1:] if len(sys.argv) > 1 else []

        # Set sys.argv to simulate "serve --log-level debug" + additional args
        sys.argv = [sys.argv[0], "serve", "--log-level", "debug"] + additional_args
        main()
    finally:
        # Restore original sys.argv
        sys.argv = original_argv


def _add_server_arguments(parser):
    """Add common server arguments to a parser"""

    # Load the persisted log level to use as a default
    persisted_log_level = load_setting("log_level", DEFAULT_LOG_LEVEL)

    parser.add_argument(
        "--port",
        type=int,
        help="Port number to serve on",
        default=DEFAULT_PORT,
    )
    parser.add_argument(
        "--host",
        type=str,
        help="Address to bind for connections",
        default=DEFAULT_HOST,
    )
    parser.add_argument(
        "--log-level",
        type=str,
        help="Log level for the server",
        choices=["critical", "error", "warning", "info", "debug", "trace"],
        default=persisted_log_level,
    )
    parser.add_argument(
        "--llamacpp",
        type=str,
        help="LlamaCpp backend to use",
        choices=["vulkan", "rocm"],
        default=DEFAULT_LLAMACPP_BACKEND,
    )
    parser.add_argument(
        "--ctx-size",
        type=int,
        help=(
            f"Context size for the model (default: {DEFAULT_CTX_SIZE} for llamacpp, "
            "truncates prompts for other recipes)"
        ),
        default=DEFAULT_CTX_SIZE,
    )

    if os.name == "nt":
        parser.add_argument(
            "--no-tray",
            action="store_true",
            help="Do not show a tray icon when the server is running",
        )


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
    _add_server_arguments(serve_parser)

    # Status command
    status_parser = subparsers.add_parser("status", help="Check if server is running")

    # Stop command
    stop_parser = subparsers.add_parser("stop", help="Stop the server")

    # List command
    list_parser = subparsers.add_parser(
        "list", help="List recommended models and their download status"
    )

    # Pull command
    pull_parser = subparsers.add_parser(
        "pull",
        help="Install an LLM",
        epilog=(
            "More information: "
            "https://github.com/lemonade-sdk/lemonade/blob/main/docs/server/server_models.md"
        ),
    )
    pull_parser.add_argument(
        "model",
        help="Lemonade Server model name",
        nargs="+",
    )
    pull_parser.add_argument(
        "--checkpoint",
        help="For registering a new model: Hugging Face checkpoint to source the model from",
    )
    pull_parser.add_argument(
        "--recipe",
        help="For registering a new model: lemonade.api recipe to use with the model",
    )
    pull_parser.add_argument(
        "--reasoning",
        help="For registering a new model: whether the model is a reasoning model or not",
        type=bool,
        default=False,
    )
    pull_parser.add_argument(
        "--mmproj",
        help="For registering a new multimodal model: full file name of the .mmproj file in the checkpoint",
    )

    # Delete command
    delete_parser = subparsers.add_parser(
        "delete",
        help="Delete an LLM",
        epilog=(
            "More information: "
            "https://github.com/lemonade-sdk/lemonade/blob/main/docs/server/server_models.md"
        ),
    )
    delete_parser.add_argument(
        "model",
        help="Lemonade Server model name",
        nargs="+",
    )

    # Run command
    run_parser = subparsers.add_parser(
        "run",
        help="Chat with specified model (starts server if needed)",
    )
    run_parser.add_argument(
        "model",
        help="Lemonade Server model name to run",
    )
    _add_server_arguments(run_parser)

    args = parser.parse_args()

    if os.name != "nt":
        args.no_tray = True

    if args.version:
        version()
    elif args.command == "serve":
        _, running_port = get_server_info()
        if running_port is not None:
            print(
                (
                    f"Lemonade Server is already running on port {running_port}\n"
                    "Please stop the existing server before starting a new instance."
                ),
            )
            sys.exit(ExitCodes.SERVER_ALREADY_RUNNING)
        serve(
            port=args.port,
            host=args.host,
            log_level=args.log_level,
            tray=not args.no_tray,
            llamacpp_backend=args.llamacpp,
            ctx_size=args.ctx_size,
        )
    elif args.command == "status":
        status()
    elif args.command == "list":
        list_models()
    elif args.command == "pull":
        pull(
            args.model,
            checkpoint=args.checkpoint,
            recipe=args.recipe,
            reasoning=args.reasoning,
            mmproj=args.mmproj,
        )
    elif args.command == "delete":
        delete(args.model)
    elif args.command == "stop":
        stop()
    elif args.command == "run":
        run(
            args.model,
            port=args.port,
            host=args.host,
            log_level=args.log_level,
            tray=not args.no_tray,
            llamacpp_backend=args.llamacpp,
            ctx_size=args.ctx_size,
        )
    elif args.command == "help" or not args.command:
        parser.print_help()


if __name__ == "__main__":
    main()

# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD

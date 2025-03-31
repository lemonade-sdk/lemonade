import argparse
import sys
import os
import psutil


def serve(args):
    """
    Execute the serve command
    """

    # Check if Lemonade Server is already running
    running_on_port = get_server_port()
    if running_on_port is not None:
        print(
            (
                f"Lemonade Server is already running on port {running_on_port}\n"
                "Please stop the existing server before starting a new instance."
            ),
        )
        return

    # Otherwise, start the server
    print("Starting Lemonade Server...")
    from lemonade.tools.serve import Server

    server = Server()
    server.run()


def version():
    """
    Print the version number
    """
    from turnkeyml import __version__ as version_number

    print(f"Lemonade Server version is {version_number}")


def status():
    """
    Print the status of the server
    """
    port = get_server_port()
    if port is None:
        print("Server is not running")
    else:
        print(f"Server is running on port {port}")


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


def get_server_port() -> int | None:
    """
    Get the port that Lemonade Server is running on
    """
    # Go over all python processes that have a port open
    for process in psutil.process_iter(["pid", "name"]):
        try:
            connections = process.net_connections()
            for conn in connections:
                if conn.status == "LISTEN":
                    if is_lemonade_server(process.info["pid"]):
                        return conn.laddr.port
        except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
            continue

    return None


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

    # Serve commands
    serve_parser = subparsers.add_parser("serve", help="Start server")
    status_parser = subparsers.add_parser("status", help="Check if server is running")

    args = parser.parse_args()

    if args.version:
        version()
    elif args.command == "serve":
        serve(args)
    elif args.command == "status":
        status()
    elif args.command == "help" or not args.command:
        parser.print_help()


if __name__ == "__main__":
    main()

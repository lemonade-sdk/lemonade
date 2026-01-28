"""
Shared base functionality for server testing.

This module contains the common setup, cleanup, and utility functions
used by all lemonade server test files.

Supports two server lifecycle modes:
- Class-level (default): Start server in setUpClass(), stop in tearDownClass()
- Per-test mode (--server-per-test): Start/stop server for each test method
"""

import unittest
import subprocess
import socket
import time
import sys
import io
import os
import argparse
from threading import Thread

try:
    from openai import OpenAI, AsyncOpenAI
except ImportError as e:
    raise ImportError("You must `pip install openai` to run this test", e)

try:
    import httpx
except ImportError:
    httpx = None

try:
    import requests
except ImportError:
    requests = None

from .capabilities import (
    set_current_config,
    get_capabilities,
    get_test_model,
    WRAPPED_SERVER_CAPABILITIES,
)
from .test_models import (
    PORT,
    STANDARD_MESSAGES,
    TIMEOUT_MODEL_OPERATION,
    TIMEOUT_DEFAULT,
    get_default_server_binary,
)

# Global configuration set by parse_args()
_config = {
    "server_binary": None,
    "wrapped_server": None,
    "backend": None,
    "server_per_test": False,
    "offline": False,
    "additional_server_args": [],
}


def parse_args(additional_args=None):
    """
    Parse command line arguments for test configuration.

    Args:
        additional_args: List of additional arguments to add to the server command

    Returns:
        Parsed args namespace
    """
    parser = argparse.ArgumentParser(description="Test lemonade server", add_help=False)
    parser.add_argument(
        "--offline",
        action="store_true",
        help="Run tests in offline mode",
    )
    parser.add_argument(
        "--server-binary",
        type=str,
        default=get_default_server_binary(),
        help="Path to server binary (default: lemonade-server in venv)",
    )
    parser.add_argument(
        "--wrapped-server",
        type=str,
        choices=list(WRAPPED_SERVER_CAPABILITIES.keys()),
        help="Which wrapped server to test (llamacpp, ryzenai, flm, etc.)",
    )
    parser.add_argument(
        "--backend",
        type=str,
        help="Backend for the wrapped server (vulkan, rocm, cpu, hybrid, npu, etc.)",
    )
    parser.add_argument(
        "--server-per-test",
        action="store_true",
        help="Start/stop server for each test instead of once per class",
    )

    # Use parse_known_args to ignore unittest arguments
    args, unknown = parser.parse_known_args()

    # Update global config
    _config["server_binary"] = args.server_binary
    _config["wrapped_server"] = args.wrapped_server
    _config["backend"] = args.backend
    _config["server_per_test"] = args.server_per_test
    _config["offline"] = args.offline
    _config["additional_server_args"] = additional_args or []

    # Set current config for capability checks
    if args.wrapped_server:
        set_current_config(args.wrapped_server, args.backend)

    return args


def get_config():
    """Get the current test configuration."""
    return _config.copy()


def get_server_binary():
    """Get the server binary path."""
    return _config["server_binary"]


def stop_lemonade():
    """Kill the lemonade server and stop the model."""
    print("\n=== Stopping Lemonade ===")
    server_binary = _config["server_binary"]

    if server_binary is None:
        print("No server binary configured, skipping stop")
        return

    try:
        result = subprocess.run(
            [server_binary, "stop"],
            capture_output=True,
            text=True,
            timeout=30,
        )
        print(result.stdout)
        if result.stderr:
            print(f"stderr: {result.stderr}")
    except subprocess.TimeoutExpired:
        print("Warning: stop command timed out")
    except Exception as e:
        print(f"Warning: failed to stop server: {e}")


def wait_for_server(port=PORT, timeout=60):
    """
    Wait for the server to start by checking if the port is available.

    Args:
        port: Port number to check
        timeout: Maximum time to wait in seconds

    Returns:
        True if server started, raises TimeoutError otherwise
    """
    start_time = time.time()
    while True:
        if time.time() - start_time > timeout:
            raise TimeoutError(f"Server failed to start within {timeout} seconds")
        try:
            conn = socket.create_connection(("localhost", port))
            conn.close()
            return True
        except socket.error:
            time.sleep(1)


def start_server(
    server_binary=None,
    wrapped_server=None,
    backend=None,
    additional_args=None,
    port=PORT,
):
    """
    Start the lemonade server.

    Args:
        server_binary: Path to server binary (uses config if None)
        wrapped_server: Wrapped server type (uses config if None)
        backend: Backend for wrapped server (uses config if None)
        additional_args: Additional arguments for the server
        port: Port to run on

    Returns:
        The subprocess.Popen object for the server process
    """
    if server_binary is None:
        server_binary = _config["server_binary"]
    if wrapped_server is None:
        wrapped_server = _config["wrapped_server"]
    if backend is None:
        backend = _config["backend"]
    if additional_args is None:
        additional_args = _config.get("additional_server_args", [])

    # Build the command
    cmd = [server_binary, "serve"]

    # Add --no-tray option on Windows or in CI environments
    # The tray app requires a display server (X11/Wayland) which isn't available in CI containers
    if os.name == "nt" or os.getenv("LEMONADE_CI_MODE"):
        cmd.append("--no-tray")

    # Add debug logging for CI environments
    cmd.extend(["--log-level", "debug"])

    # Add port if not default
    if port != PORT:
        cmd.extend(["--port", str(port)])

    # Add llamacpp backend option if specified
    if wrapped_server == "llamacpp" and backend:
        cmd.extend(["--llamacpp", backend])

    # Add any additional server arguments
    if additional_args:
        cmd.extend(additional_args)

    print(f"Starting server: {' '.join(cmd)}")

    # Start the server process
    process = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
        encoding="utf-8",
        errors="replace",
        env=os.environ.copy(),
    )

    # Print stdout and stderr in real-time using daemon threads
    def print_stdout():
        try:
            for line in process.stdout:
                print(f"[stdout] {line.strip()}")
        except Exception:
            pass

    def print_stderr():
        try:
            for line in process.stderr:
                print(f"[stderr] {line.strip()}")
        except Exception:
            pass

    stdout_thread = Thread(target=print_stdout, daemon=True)
    stderr_thread = Thread(target=print_stderr, daemon=True)
    stdout_thread.start()
    stderr_thread.start()

    # Wait for the server to start
    wait_for_server(port)

    # Additional wait for server to fully initialize
    time.sleep(5)

    print("Server started successfully")

    return process


class ServerTestBase(unittest.IsolatedAsyncioTestCase):
    """
    Base class for server tests.

    Supports two lifecycle modes controlled by --server-per-test flag:
    - Class-level (default): Server starts once for all tests in the class
    - Per-test mode: Server starts fresh for each test method

    Subclasses should not override setUpClass/tearDownClass directly.
    Instead, use class variables to configure behavior:
    - additional_server_args: List of extra args to pass to server
    """

    # Class-level server process (used in class-level mode)
    _server_process = None

    # Configuration
    additional_server_args = []

    @classmethod
    def setUpClass(cls):
        """Start server if using class-level mode."""
        super().setUpClass()

        # Ensure stdout can handle Unicode
        if sys.stdout.encoding != "utf-8":
            sys.stdout = io.TextIOWrapper(
                sys.stdout.buffer, encoding="utf-8", errors="replace"
            )
            sys.stderr = io.TextIOWrapper(
                sys.stderr.buffer, encoding="utf-8", errors="replace"
            )

        # Stop any existing server
        stop_lemonade()

        # Start server if not in per-test mode
        if not _config.get("server_per_test", False):
            all_args = (
                _config.get("additional_server_args", []) + cls.additional_server_args
            )
            cls._server_process = start_server(additional_args=all_args)

    @classmethod
    def tearDownClass(cls):
        """Stop server. Always stops regardless of mode to ensure cleanup."""
        # Always stop the server to ensure no orphaned processes
        stop_lemonade()
        cls._server_process = None
        super().tearDownClass()

    def setUp(self):
        """Set up for each test. Starts server if in per-test mode."""
        print(f"\n=== Starting test: {self._testMethodName} ===")

        self.base_url = f"http://localhost:{PORT}/api/v1"
        self.messages = STANDARD_MESSAGES.copy()

        # Start server if in per-test mode
        if _config.get("server_per_test", False):
            stop_lemonade()
            all_args = (
                _config.get("additional_server_args", []) + self.additional_server_args
            )
            self._test_server_process = start_server(additional_args=all_args)

    def tearDown(self):
        """Clean up after each test. Stops server if in per-test mode."""
        if _config.get("server_per_test", False):
            stop_lemonade()
            self._test_server_process = None

    def get_openai_client(self) -> OpenAI:
        """Get a synchronous OpenAI client configured for the test server."""
        return OpenAI(
            base_url=self.base_url,
            api_key="lemonade",  # required but unused
            timeout=TIMEOUT_MODEL_OPERATION,  # inference may trigger model download
        )

    def get_async_openai_client(self) -> AsyncOpenAI:
        """Get an async OpenAI client configured for the test server."""
        return AsyncOpenAI(
            base_url=self.base_url,
            api_key="lemonade",  # required but unused
            timeout=TIMEOUT_MODEL_OPERATION,  # inference may trigger model download
        )

    def get_test_model(self, model_type: str = "llm") -> str:
        """
        Get the appropriate test model for the current configuration.

        Args:
            model_type: Type of model (llm, embedding, reranking, etc.)

        Returns:
            Model name string
        """
        return get_test_model(model_type)


def run_server_tests(
    test_class,
    description="SERVER TESTS",
    wrapped_server=None,
    backend=None,
    additional_args=None,
):
    """
    Run server tests with the given test class.

    IMPORTANT: This function ensures the server is ALWAYS stopped before exiting,
    regardless of whether tests passed or failed.

    Args:
        test_class: The unittest.TestCase class to run
        description: Description for the test run
        wrapped_server: Override wrapped server from command line
        backend: Override backend from command line
        additional_args: Additional args to pass to server
    """
    # Parse args and configure
    args = parse_args(additional_args)

    # Allow overrides
    if wrapped_server:
        _config["wrapped_server"] = wrapped_server
        set_current_config(wrapped_server, backend or _config["backend"])
    if backend:
        _config["backend"] = backend

    ws = _config.get("wrapped_server", "unknown")
    be = _config.get("backend", "default")
    mode = "per-test" if _config.get("server_per_test") else "class-level"

    print(f"\n{'=' * 70}")
    print(f"{description}")
    print(f"Wrapped Server: {ws}, Backend: {be}")
    print(f"Server Lifecycle: {mode}")
    print(f"{'=' * 70}\n")

    result = None
    try:
        # Create and run test suite
        loader = unittest.TestLoader()
        suite = loader.loadTestsFromTestCase(test_class)

        runner = unittest.TextTestRunner(verbosity=2, buffer=False, failfast=True)
        result = runner.run(suite)
    finally:
        # ALWAYS stop the server before exiting, regardless of test outcome
        print("\n=== Final cleanup: ensuring server is stopped ===")
        stop_lemonade()

    # Exit with appropriate code
    sys.exit(0 if (result and result.wasSuccessful()) else 1)


# Re-export commonly used items
__all__ = [
    "ServerTestBase",
    "parse_args",
    "get_config",
    "get_server_binary",
    "stop_lemonade",
    "wait_for_server",
    "start_server",
    "run_server_tests",
    "OpenAI",
    "AsyncOpenAI",
    "httpx",
    "requests",
    "PORT",
]

"""
Shared base functionality for server testing.

This module contains the common setup, cleanup, and utility functions
used by all lemonade server test files.

Tests expect a running server (started by the installer or manually).
ServerTestBase.setUpClass() verifies the server is reachable and applies
runtime configuration via POST /internal/set.
"""

import unittest
import socket
import time
import sys
import io
import os
import argparse

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
    get_all_wrapped_server_names,
    get_wrapped_servers_for_modality,
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
    "modality": None,
    "offline": False,
    "additional_server_args": [],
}


def parse_args(additional_args=None, modality=None):
    """
    Parse command line arguments for test configuration.

    Args:
        additional_args: List of additional arguments to add to the server command
        modality: Modality key (e.g., "llm", "whisper", "stable_diffusion").
                  When set, validates --wrapped-server against that modality's backends.

    Returns:
        Parsed args namespace
    """
    # Determine valid --wrapped-server choices based on modality
    if modality:
        valid_servers = sorted(get_wrapped_servers_for_modality(modality))
    else:
        valid_servers = sorted(get_all_wrapped_server_names())

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
        choices=valid_servers,
        help="Which wrapped server to test (llamacpp, ryzenai, flm, etc.)",
    )
    parser.add_argument(
        "--backend",
        type=str,
        help="Backend for the wrapped server (vulkan, rocm, cpu, hybrid, npu, etc.)",
    )

    # Use parse_known_args to ignore unittest arguments
    args, unknown = parser.parse_known_args()

    # Update global config
    _config["server_binary"] = args.server_binary
    _config["wrapped_server"] = args.wrapped_server
    _config["backend"] = args.backend
    _config["modality"] = modality
    _config["offline"] = args.offline
    _config["additional_server_args"] = additional_args or []

    # Set current config for capability checks
    if args.wrapped_server:
        set_current_config(args.wrapped_server, args.backend, modality)

    return args


def get_config():
    """Get the current test configuration."""
    return _config.copy()


def get_server_binary():
    """Get the server binary path."""
    return _config["server_binary"]


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


def _auth_headers():
    """Return Authorization header if LEMONADE_API_KEY is set."""
    api_key = os.environ.get("LEMONADE_API_KEY")
    if api_key:
        return {"Authorization": f"Bearer {api_key}"}
    return {}


def set_server_config(config: dict, port=PORT):
    """POST /internal/set to update server config at runtime."""
    response = requests.post(
        f"http://localhost:{port}/internal/set",
        json=config,
        headers=_auth_headers(),
        timeout=10,
    )
    response.raise_for_status()
    return response.json()


def unload_all_models(port=PORT):
    """POST /api/v1/unload to unload all models for clean state."""
    response = requests.post(
        f"http://localhost:{port}/api/v1/unload",
        json={},
        headers=_auth_headers(),
        timeout=30,
    )
    # 200 = unloaded, 404 = nothing loaded — both OK
    return response


def _build_runtime_config(additional_server_args=None):
    """
    Translate CLI args (--wrapped-server, --backend, additional_server_args)
    into a dict suitable for POST /internal/set.

    Args:
        additional_server_args: Extra args to parse (merged with global config's args).
                                If None, uses only the global config's args.
    """
    config = {}

    wrapped_server = _config.get("wrapped_server")
    backend = _config.get("backend")

    # Map --wrapped-server + --backend to the correct recipe option key
    if wrapped_server == "llamacpp" and backend:
        config["llamacpp_backend"] = backend
    elif wrapped_server == "sd-cpp" and backend:
        config["sd-cpp_backend"] = backend
    elif wrapped_server == "whispercpp" and backend:
        config["whispercpp_backend"] = backend

    # Parse additional_server_args for known flags
    additional = list(_config.get("additional_server_args", []))
    if additional_server_args:
        additional.extend(additional_server_args)

    i = 0
    while i < len(additional):
        arg = additional[i]
        if arg == "--max-loaded-models" and i + 1 < len(additional):
            config["max_loaded_models"] = int(additional[i + 1])
            i += 2
        elif arg == "--llamacpp" and i + 1 < len(additional):
            config["llamacpp_backend"] = additional[i + 1]
            i += 2
        elif arg == "--sdcpp" and i + 1 < len(additional):
            config["sd-cpp_backend"] = additional[i + 1]
            i += 2
        elif arg == "--whispercpp" and i + 1 < len(additional):
            config["whispercpp_backend"] = additional[i + 1]
            i += 2
        elif arg == "--ctx-size" and i + 1 < len(additional):
            config["ctx_size"] = int(additional[i + 1])
            i += 2
        elif arg == "--log-level" and i + 1 < len(additional):
            config["log_level"] = additional[i + 1]
            i += 2
        else:
            i += 1

    return config


class ServerTestBase(unittest.TestCase):
    """
    Base class for server tests.

    Expects a running server (started by the installer or manually).
    setUpClass() verifies the server is reachable and applies runtime config.
    No server start/stop is performed.

    Subclasses can set class variables to configure behavior:
    - additional_server_args: List of extra args translated to /internal/set calls
    """

    # Configuration
    additional_server_args = []

    @classmethod
    def setUpClass(cls):
        """Verify server is reachable and apply runtime configuration."""
        super().setUpClass()

        # Ensure stdout can handle Unicode
        if sys.stdout.encoding != "utf-8":
            sys.stdout = io.TextIOWrapper(
                sys.stdout.buffer, encoding="utf-8", errors="replace"
            )
            sys.stderr = io.TextIOWrapper(
                sys.stderr.buffer, encoding="utf-8", errors="replace"
            )

        # Verify server is running
        print("\n=== Verifying server is reachable ===")
        try:
            wait_for_server(timeout=30)
        except TimeoutError:
            raise RuntimeError(
                "Server is not running on port %d. "
                "Start the server before running tests "
                "(e.g., install the package or run lemonade-router manually)." % PORT
            )
        print("Server is reachable on port %d" % PORT)

        # Build and apply runtime config from CLI args + class-level args
        runtime_config = _build_runtime_config(cls.additional_server_args)

        if runtime_config:
            print(f"Applying runtime config: {runtime_config}")
            try:
                set_server_config(runtime_config)
            except Exception as e:
                print(f"Warning: Failed to apply runtime config: {e}")

        # Unload all models for clean state
        print("Unloading all models for clean state...")
        try:
            unload_all_models()
        except Exception as e:
            print(f"Warning: Failed to unload models: {e}")

    @classmethod
    def tearDownClass(cls):
        """No server lifecycle management needed."""
        super().tearDownClass()

    def setUp(self):
        """Set up for each test."""
        print(f"\n=== Starting test: {self._testMethodName} ===")

        self.base_url = f"http://localhost:{PORT}/api/v1"
        self.messages = STANDARD_MESSAGES.copy()

    def tearDown(self):
        """Clean up after each test."""
        pass

    def get_openai_client(self) -> OpenAI:
        """Get a synchronous OpenAI client configured for the test server."""
        return OpenAI(
            base_url=self.base_url,
            api_key=os.environ.get("LEMONADE_API_KEY", "lemonade"),
            timeout=TIMEOUT_MODEL_OPERATION,  # inference may trigger model download
        )

    def get_async_openai_client(self) -> AsyncOpenAI:
        """Get an async OpenAI client configured for the test server."""
        return AsyncOpenAI(
            base_url=self.base_url,
            api_key=os.environ.get("LEMONADE_API_KEY", "lemonade"),
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
    modality=None,
    default_wrapped_server=None,
):
    """
    Run server tests with the given test class.

    Args:
        test_class: The unittest.TestCase class to run
        description: Description for the test run
        wrapped_server: Override wrapped server from command line
        backend: Override backend from command line
        additional_args: Additional args to pass to server
        modality: Modality key (e.g., "llm", "whisper", "stable_diffusion")
        default_wrapped_server: Default wrapped server when none specified on CLI
    """
    # Parse args and configure
    args = parse_args(additional_args, modality=modality)

    # Allow overrides
    if wrapped_server:
        _config["wrapped_server"] = wrapped_server
        set_current_config(
            wrapped_server,
            backend or _config["backend"],
            modality or _config["modality"],
        )
    if backend:
        _config["backend"] = backend

    # Apply default wrapped server if none was specified via CLI or override
    if not _config["wrapped_server"] and default_wrapped_server:
        _config["wrapped_server"] = default_wrapped_server
        set_current_config(
            default_wrapped_server,
            _config["backend"],
            _config["modality"],
        )

    ws = _config.get("wrapped_server", "unknown")
    be = _config.get("backend", "default")

    print(f"\n{'=' * 70}")
    print(f"{description}")
    print(f"Wrapped Server: {ws}, Backend: {be}")
    print(f"{'=' * 70}\n")

    # Create and run test suite
    loader = unittest.TestLoader()
    suite = loader.loadTestsFromTestCase(test_class)

    runner = unittest.TextTestRunner(verbosity=2, buffer=False, failfast=True)
    result = runner.run(suite)

    # Exit with appropriate code
    sys.exit(0 if (result and result.wasSuccessful()) else 1)


# Re-export commonly used items
__all__ = [
    "ServerTestBase",
    "parse_args",
    "get_config",
    "get_server_binary",
    "wait_for_server",
    "set_server_config",
    "unload_all_models",
    "run_server_tests",
    "OpenAI",
    "AsyncOpenAI",
    "httpx",
    "requests",
    "PORT",
]

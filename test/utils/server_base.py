"""
Shared base functionality for server testing.

This module contains the common setup, cleanup, and utility functions
used by both server.py and server_llamacpp.py tests.
"""

import unittest
import subprocess
import psutil
import asyncio
import socket
import time
from threading import Thread
import sys
import io
import httpx
import argparse
import contextlib
from unittest.mock import patch
import urllib.request
import os
import requests

try:
    from openai import OpenAI, AsyncOpenAI
except ImportError as e:
    raise ImportError("You must `pip install openai` to run this test", e)

# Import huggingface_hub for patching in offline mode
try:
    from huggingface_hub import snapshot_download as original_snapshot_download
except ImportError:
    # If huggingface_hub is not installed, create a dummy function
    def original_snapshot_download(*args, **kwargs):
        raise ImportError("huggingface_hub is not installed")


MODEL_NAME = "Qwen2.5-0.5B-Instruct-CPU"
# This list must include all models that could be accessed in offline testing
MODELS_UNDER_TEST = [
    MODEL_NAME,
    "Llama-3.2-1B-Instruct-CPU",  # used in test_001_test_simultaneous_load_requests
]
MODEL_CHECKPOINT = "amd/Qwen2.5-0.5B-Instruct-quantized_int4-float16-cpu-onnx"
PORT = 8000


def parse_args():
    """Parse command line arguments."""
    parser = argparse.ArgumentParser(description="Test lemonade server")
    parser.add_argument(
        "--offline", action="store_true", help="Run tests in offline mode"
    )
    return parser.parse_args()


@contextlib.contextmanager
def simulate_offline_mode():
    """
    Context manager that simulates a fully offline environment except
    for local connections needed for testing.

    This patches multiple network-related functions to prevent any
    external network access during tests.
    """
    original_create_connection = socket.create_connection

    def mock_create_connection(address, *args, **kwargs):
        host, port = address
        # Allow connections to localhost for testing
        if host == "localhost" or host == "127.0.0.1":
            return original_create_connection(address, *args, **kwargs)
        # Block all other connections
        raise socket.error("Network access disabled for offline testing")

    # Define a function that raises an error for non-local requests
    def block_external_requests(original_func):
        def wrapper(url, *args, **kwargs):
            # Allow localhost requests
            if url.startswith(
                (
                    "http://localhost",
                    "https://localhost",
                    "http://127.0.0.1",
                    "https://127.0.0.1",
                )
            ):
                return original_func(url, *args, **kwargs)
            raise ConnectionError(f"Offline mode: network request blocked to {url}")

        return wrapper

    # Apply all necessary patches to simulate offline mode
    with patch("socket.create_connection", side_effect=mock_create_connection):
        with patch(
            "huggingface_hub.snapshot_download",
            side_effect=lambda *args, **kwargs: (
                kwargs.get("local_files_only", False)
                and original_snapshot_download(*args, **kwargs)
                or (_ for _ in ()).throw(
                    ValueError("Offline mode: network connection attempted")
                )
            ),
        ):
            # Also patch urllib and requests to block external requests
            with patch(
                "urllib.request.urlopen",
                side_effect=block_external_requests(urllib.request.urlopen),
            ):
                with patch(
                    "http.client.HTTPConnection.connect",
                    side_effect=lambda self, *args, **kwargs: (
                        None
                        if self.host in ("localhost", "127.0.0.1")
                        else (_ for _ in ()).throw(
                            ConnectionError("Offline mode: connection blocked")
                        )
                    ),
                ):
                    # Set environment variable to signal offline mode
                    os.environ["LEMONADE_OFFLINE_TEST"] = "1"
                    try:
                        yield
                    finally:
                        # Clean up environment variable
                        if "LEMONADE_OFFLINE_TEST" in os.environ:
                            del os.environ["LEMONADE_OFFLINE_TEST"]


def ensure_model_is_cached():
    """
    Make sure the test model is downloaded and cached locally before running in offline mode.
    """
    try:
        # Call lemonade-server-dev pull to download the model
        for model_name in MODELS_UNDER_TEST:
            subprocess.run(
                ["lemonade-server-dev", "pull", model_name],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=True,
            )
            print(f"Model {model_name} successfully pulled and available in cache")
        return True
    except subprocess.CalledProcessError as e:
        print(f"Failed to download model: {e}")
        return False


def kill_process_on_port(port):
    """Kill any process that is using the specified port."""
    killed = False
    for proc in psutil.process_iter(["pid", "name"]):
        try:
            connections = proc.net_connections()
            for conn in connections:
                if conn.laddr.port == port:
                    proc_name = proc.name()
                    proc_pid = proc.pid
                    proc.kill()
                    print(
                        f"Killed process {proc_name} (PID: {proc_pid}) using port {port}"
                    )
                    killed = True
        except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess):
            continue

    if not killed:
        print(f"No process found using port {port}")


class ServerTestingBase(unittest.IsolatedAsyncioTestCase):
    """Base class containing only shared setup/cleanup functionality, no test methods."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        # Allow subclasses to set the llamacpp backend
        self.llamacpp_backend = getattr(self, "llamacpp_backend", None)

    def setUp(self):
        """
        Start lemonade server process
        """
        print("\n=== Starting new test ===")
        self.base_url = f"http://localhost:{PORT}/api/v1"
        self.messages = [
            {"role": "system", "content": "You are a helpful assistant."},
            {"role": "user", "content": "Who won the world series in 2020?"},
            {"role": "assistant", "content": "The LA Dodgers won in 2020."},
            {"role": "user", "content": "In which state was it played?"},
        ]

        # Ensure we kill anything using port 8000
        kill_process_on_port(PORT)

        # Build the command to start the server
        cmd = ["lemonade-server-dev", "serve"]

        # Add --no-tray option on Windows
        if os.name == "nt":
            cmd.append("--no-tray")

        # Add llamacpp backend option if specified
        if self.llamacpp_backend:
            cmd.extend(["--llamacpp", self.llamacpp_backend])

        # Start the lemonade server
        lemonade_process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )

        # Print stdout and stderr in real-time
        def print_output():
            while True:
                stdout = lemonade_process.stdout.readline()
                stderr = lemonade_process.stderr.readline()
                if stdout:
                    print(f"[stdout] {stdout.strip()}")
                if stderr:
                    print(f"[stderr] {stderr.strip()}")
                if not stdout and not stderr and lemonade_process.poll() is not None:
                    break

        output_thread = Thread(target=print_output, daemon=True)
        output_thread.start()

        # Wait for the server to start by checking port 8000
        start_time = time.time()
        while True:
            if time.time() - start_time > 60:
                raise TimeoutError("Server failed to start within 60 seconds")
            try:
                conn = socket.create_connection(("localhost", PORT))
                conn.close()
                break
            except socket.error:
                time.sleep(1)

        # Wait a few other seconds after the port is available
        time.sleep(5)

        print("Server started successfully")

        self.addCleanup(self.cleanup_lemonade, lemonade_process)

        # Ensure stdout can handle Unicode
        if sys.stdout.encoding != "utf-8":
            sys.stdout = io.TextIOWrapper(
                sys.stdout.buffer, encoding="utf-8", errors="replace"
            )
            sys.stderr = io.TextIOWrapper(
                sys.stderr.buffer, encoding="utf-8", errors="replace"
            )

    def cleanup_lemonade(self, server_subprocess: subprocess.Popen):
        """
        Kill the lemonade server and stop the model
        """

        # Kill the server subprocess
        print("\n=== Cleaning up test ===")

        parent = psutil.Process(server_subprocess.pid)
        for child in parent.children(recursive=True):
            child.kill()

        server_subprocess.kill()

        kill_process_on_port(PORT)


def run_server_tests_with_class(test_class, description="SERVER TESTS", offline=None):
    """Utility function to run server tests with a given test class."""
    # If offline parameter is not provided, use argparse to get it
    if offline is None:
        args = parse_args()
        offline = args.offline

    if offline:
        print(f"\n=== STARTING {description} IN OFFLINE MODE ===")

        if not ensure_model_is_cached():
            print("ERROR: Unable to cache the model needed for offline testing")
            sys.exit(1)

        print("Model is cached. Running tests with network access disabled...")

        # Create a new test suite
        test_loader = unittest.TestLoader()
        test_suite = test_loader.loadTestsFromTestCase(test_class)

        # Run the tests in offline mode
        with simulate_offline_mode():
            result = unittest.TextTestRunner().run(test_suite)

        # Set exit code based on test results
        sys.exit(0 if result.wasSuccessful() else 1)
    else:
        print(f"\n=== STARTING {description} IN NORMAL MODE ===")
        # Create a new test suite for the specific class
        test_loader = unittest.TestLoader()
        test_suite = test_loader.loadTestsFromTestCase(test_class)
        unittest.TextTestRunner().run(test_suite)


# This file was originally licensed under Apache 2.0. It has been modified.
# Modifications Copyright (c) 2025 AMD

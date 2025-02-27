"""
Usage: python server.py

This will launch the lemonade server, query it in openai mode,
and make sure that the response is valid.

If you get the `ImportError: cannot import name 'TypeIs' from 'typing_extensions'` error:
    1. pip uninstall typing_extensions
    2. pip install openai
"""

import unittest
import subprocess
import psutil
import asyncio
import socket
import time
from threading import Thread

try:
    from openai import OpenAI, AsyncOpenAI
except ImportError as e:
    raise ImportError("You must `pip install openai` to run this test", e)

MODEL = "Qwen/Qwen2.5-1.5B-Instruct"
PORT = 8000


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


class Testing(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        """
        Start lemonade server process
        """
        print("\n=== Starting new test ===")
        self.base_url = f"http://localhost:{PORT}/api/v0"
        self.messages = [
            {"role": "system", "content": "You are a helpful assistant."},
            {"role": "user", "content": "Who won the world series in 2020?"},
            {"role": "assistant", "content": "The LA Dodgers won in 2020."},
            {"role": "user", "content": "In which state was it played?"},
        ]

        # Ensure we kill anything using port 8000
        kill_process_on_port(PORT)

        # Start the lemonade server
        lemonade_process = subprocess.Popen(
            ["lemonade", "serve"],
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

    def test_001_test_chat_completion(self):
        client = OpenAI(
            base_url=self.base_url,
            api_key="lemonade",  # required, but unused
        )

        completion = client.chat.completions.create(
            model=MODEL,
            messages=self.messages,
        )

        print(completion.choices[0].message.content)
        assert len(completion.choices[0].message.content) > 5

    def test_002_test_chat_completion_streaming(self):
        client = OpenAI(
            base_url=self.base_url,
            api_key="lemonade",  # required, but unused
        )

        stream = client.chat.completions.create(
            model=MODEL,
            messages=self.messages,
            stream=True,
        )
        complete_response = ""
        chunk_count = 0
        for chunk in stream:
            if chunk.choices[0].delta.content is not None:
                complete_response += chunk.choices[0].delta.content
                print(chunk.choices[0].delta.content, end="")
                chunk_count += 1

        assert chunk_count > 5
        assert len(complete_response) > 5

    async def test_003_test_chat_completion_streaming_async(self):
        client = AsyncOpenAI(
            base_url=self.base_url,
            api_key="lemonade",  # required, but unused
        )

        complete_response = ""
        stream = await client.chat.completions.create(
            model=MODEL,
            messages=self.messages,
            stream=True,
        )

        chunk_count = 0
        async for chunk in stream:
            if chunk.choices[0].delta.content is not None:
                complete_response += chunk.choices[0].delta.content
                print(chunk.choices[0].delta.content, end="")
                chunk_count += 1

        assert chunk_count > 5
        assert len(complete_response) > 5

    def test_004_test_models(self):
        client = OpenAI(
            base_url=self.base_url,
            api_key="lemonade",  # required, but unused
        )

        # Get the list of models
        l = client.models.list()

        # Check that the list is not empty
        assert len(l.data) > 0

        # Check that the list contains the models we expect
        assert any(model.id == "Llama-3.2-1B-Instruct-Hybrid" for model in l.data)


if __name__ == "__main__":
    unittest.main()

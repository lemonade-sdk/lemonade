import json
import os
import shutil
import subprocess
import tempfile
import time
import unittest
import requests

from utils.test_models import get_default_lemond_binary, get_default_cli_binary


class TestMultiCheckpointCompleteness(unittest.TestCase):
    """
    Thin server/CLI integration coverage for multi-checkpoint completeness.

    Filesystem-state transitions are covered directly by C++ unit tests. This
    suite only verifies that an incomplete component reaches the CLI and that
    collection pull fan-out does not skip it.
    """

    def setUp(self):
        self.tmp_dir = tempfile.mkdtemp()
        self.lemond_bin = get_default_lemond_binary()
        self.cli_bin = get_default_cli_binary()
        self.port = 13306
        self.server_proc = None
        self.server_stdout = ""
        self.server_stderr = ""

    def tearDown(self):
        self.stop_server()
        shutil.rmtree(self.tmp_dir)

    def start_server(self, capture_output=False):
        self.stop_server()
        env = os.environ.copy()
        # Ensure it doesn't use the real HF cache
        env["HF_HUB_CACHE"] = os.path.join(self.tmp_dir, "hf")
        os.makedirs(env["HF_HUB_CACHE"], exist_ok=True)

        stdout = subprocess.PIPE if capture_output else subprocess.DEVNULL
        stderr = subprocess.PIPE if capture_output else subprocess.DEVNULL

        self.server_proc = subprocess.Popen(
            [self.lemond_bin, self.tmp_dir, "--port", str(self.port)],
            stdout=stdout,
            stderr=stderr,
            text=True,
            env=env,
        )
        for i in range(30):
            try:
                requests.get(f"http://localhost:{self.port}/api/v1/models", timeout=1)
                break
            except:
                time.sleep(1)
        else:
            self.fail("Server timed out")

    def stop_server(self):
        if self.server_proc:
            self.server_proc.terminate()
            try:
                stdout, stderr = self.server_proc.communicate(timeout=5)
                self.server_stdout = stdout if stdout else ""
                self.server_stderr = stderr if stderr else ""
            except:
                self.server_proc.kill()
                stdout, stderr = self.server_proc.communicate()
                self.server_stdout = stdout if stdout else ""
                self.server_stderr = stderr if stderr else ""
            self.server_proc = None

    def test_incomplete_component_reaches_cli_and_collection_pull(self):
        comp_id = "comp-multi"
        coll_id = "coll-test"
        path1 = os.path.join(self.tmp_dir, "model1.gguf")
        path2 = os.path.join(self.tmp_dir, "model2.gguf")

        user_models = {
            comp_id: {
                "checkpoints": {"main": path1, "vae": path2},
                "recipe": "llamacpp",
                "source": "local_path",
            },
            coll_id: {"components": [comp_id], "recipe": "collection.omni"},
        }

        with open(os.path.join(self.tmp_dir, "user_models.json"), "w") as f:
            json.dump(user_models, f)

        with open(path1, "wb") as f:
            f.write(b"GGUF")

        self.start_server(capture_output=True)

        list_result = subprocess.run(
            [self.cli_bin, "--port", str(self.port), "list"],
            capture_output=True,
            text=True,
        )
        self.assertEqual(list_result.returncode, 0, list_result.stderr)
        component_rows = [
            line for line in list_result.stdout.splitlines() if comp_id in line
        ]
        self.assertTrue(component_rows, list_result.stdout)
        self.assertIn("No", component_rows[0])

        subprocess.run(
            [self.cli_bin, "--port", str(self.port), "pull", coll_id],
            capture_output=True,
            text=True,
        )

        self.stop_server()

        log_output = self.server_stdout + self.server_stderr
        self.assertIn(f"Downloading component: {comp_id}", log_output)


if __name__ == "__main__":
    unittest.main()

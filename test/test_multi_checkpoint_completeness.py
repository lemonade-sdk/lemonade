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
    Integration tests for multi-checkpoint download integrity.

    These tests verify that models requiring multiple files (e.g., Image Gen with VAEs)
    are only marked as 'Downloaded' when all required components are 100% present.

    Operation:
    - Creates a temporary workspace for each test case.
    - Simulates 'broken' or 'partial' downloads by creating fake files and marker files
      (.partial, .download_manifest.json) on disk.
    - Launches the 'lemond' server and uses the 'lemonade' CLI to verify the
      reported download status matches expectations.
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

    def test_multi_checkpoint_completeness(self):
        model_id = "multi-test"
        path1 = os.path.join(self.tmp_dir, "model1.gguf")
        path2 = os.path.join(self.tmp_dir, "model2.gguf")

        user_models = {
            model_id: {
                "checkpoints": {"main": path1, "vae": path2},
                "recipe": "llamacpp",
                "source": "local_path",
            }
        }

        with open(os.path.join(self.tmp_dir, "user_models.json"), "w") as f:
            json.dump(user_models, f)

        # 1. No files
        self.start_server()
        res = subprocess.run(
            [self.cli_bin, "--port", str(self.port), "list"],
            capture_output=True,
            text=True,
        )
        self.assertIn("No", [l for l in res.stdout.splitlines() if model_id in l][0])

        # 2. One file
        with open(path1, "w") as f:
            f.write("fake")
        self.start_server()
        res = subprocess.run(
            [self.cli_bin, "--port", str(self.port), "list"],
            capture_output=True,
            text=True,
        )
        self.assertIn("No", [l for l in res.stdout.splitlines() if model_id in l][0])

        # 3. Two files
        with open(path2, "w") as f:
            f.write("fake")
        self.start_server()
        res = subprocess.run(
            [self.cli_bin, "--port", str(self.port), "list"],
            capture_output=True,
            text=True,
        )
        self.assertIn("Yes", [l for l in res.stdout.splitlines() if model_id in l][0])

        # 4. Partial file
        partial = path1 + ".partial"
        with open(partial, "w") as f:
            f.write("partial")
        self.start_server()
        res = subprocess.run(
            [self.cli_bin, "--port", str(self.port), "list"],
            capture_output=True,
            text=True,
        )
        self.assertIn("No", [l for l in res.stdout.splitlines() if model_id in l][0])
        os.remove(partial)

        # 5. HF Marker logic
        # We'll use a fake HF repo structure
        hf_model_id = "hf-test"
        repo = "org/repo"
        # models--org--repo/snapshots/main/model.gguf
        repo_dir = os.path.join(self.tmp_dir, "hf", "models--org--repo")
        snapshot_dir = os.path.join(repo_dir, "snapshots", "main")
        os.makedirs(snapshot_dir, exist_ok=True)
        gguf_path = os.path.join(snapshot_dir, "model.gguf")
        with open(gguf_path, "w") as f:
            f.write("fake")

        # Register it
        with open(os.path.join(self.tmp_dir, "user_models.json"), "r") as f:
            user_models = json.load(f)
        user_models[hf_model_id] = {
            "checkpoint": f"{repo}:model.gguf",
            "recipe": "llamacpp",
        }
        with open(os.path.join(self.tmp_dir, "user_models.json"), "w") as f:
            json.dump(user_models, f)

        # Should be Yes initially
        self.start_server()
        res = subprocess.run(
            [self.cli_bin, "--port", str(self.port), "list"],
            capture_output=True,
            text=True,
        )
        self.assertIn(
            "Yes", [l for l in res.stdout.splitlines() if hf_model_id in l][0]
        )

        # Add manifest at snapshot root
        manifest = os.path.join(snapshot_dir, ".download_manifest.json")
        with open(manifest, "w") as f:
            f.write("{}")
        self.start_server()
        res = subprocess.run(
            [self.cli_bin, "--port", str(self.port), "list"],
            capture_output=True,
            text=True,
        )
        self.assertIn("No", [l for l in res.stdout.splitlines() if hf_model_id in l][0])

    def test_collection_status_with_incomplete_component(self):
        # Status-regression coverage for the collection fan-out skip predicate.
        # If a component is missing an auxiliary checkpoint, it must not be
        # reported as downloaded; collection pull uses that downloaded status
        # to decide whether a component can be skipped.
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

        # 1. Component incomplete (only 1 file)
        with open(path1, "w") as f:
            f.write("fake")

        # Start server and capture output to verify fan-out logs
        self.start_server(capture_output=True)

        # 2. Run 'pull' on the collection.
        # The pull subprocess return code is intentionally not asserted here
        # because the fake local-path component may fail later. This test
        # only asserts that collection fan-out reaches the incomplete component,
        # not that the full pull succeeds.
        subprocess.run(
            [self.cli_bin, "--port", str(self.port), "pull", coll_id],
            capture_output=True,
            text=True,
        )

        # Stop server to flush logs
        self.stop_server()

        # Check server logs for the fan-out decision
        log_output = self.server_stdout + self.server_stderr
        self.assertIn(f"Downloading component: {comp_id}", log_output)

    def test_single_checkpoint_guard(self):
        # Ensure standard models still work normally
        model_id = "single-test"
        path = os.path.join(self.tmp_dir, "single.gguf")

        user_models = {
            model_id: {
                "checkpoint": path,
                "recipe": "llamacpp",
                "source": "local_path",
            }
        }

        with open(os.path.join(self.tmp_dir, "user_models.json"), "w") as f:
            json.dump(user_models, f)

        # Downloaded
        with open(path, "w") as f:
            f.write("fake")
        self.start_server()
        res = subprocess.run(
            [self.cli_bin, "--port", str(self.port), "list"],
            capture_output=True,
            text=True,
        )
        self.assertIn("Yes", [l for l in res.stdout.splitlines() if model_id in l][0])

        # Partial
        with open(path + ".partial", "w") as f:
            f.write("fake")
        self.start_server()
        res = subprocess.run(
            [self.cli_bin, "--port", str(self.port), "list"],
            capture_output=True,
            text=True,
        )
        self.assertIn("No", [l for l in res.stdout.splitlines() if model_id in l][0])


if __name__ == "__main__":
    unittest.main()

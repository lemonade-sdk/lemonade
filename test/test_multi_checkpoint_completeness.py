import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from utils.server_fixture import allocate_free_port, lemond_server, make_clean_env
from utils.test_models import get_default_cli_binary


class TestMultiCheckpointCompleteness(unittest.TestCase):
    """
    Thin server/CLI integration coverage for multi-checkpoint completeness.

    Filesystem-state transitions are covered directly by C++ unit tests. This
    suite only verifies that an incomplete component reaches the CLI and that
    collection pull fan-out does not skip it.
    """

    def setUp(self):
        self.tmp_dir = tempfile.mkdtemp()
        self.cli_bin = get_default_cli_binary()
        self.port = allocate_free_port()

    def tearDown(self):
        shutil.rmtree(self.tmp_dir, ignore_errors=True)

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

        env = make_clean_env(self.tmp_dir)
        os.makedirs(env["LEMONADE_CONFIG_DIR"], exist_ok=True)
        with open(
            os.path.join(env["LEMONADE_CONFIG_DIR"], "user_models.json"), "w"
        ) as f:
            json.dump(user_models, f)

        with open(path1, "wb") as f:
            f.write(b"GGUF")

        log_path = os.path.join(self.tmp_dir, "lemond.log")
        with open(log_path, "w", encoding="utf-8") as log_file:
            with lemond_server(
                port=self.port,
                cache_dir=self.tmp_dir,
                env=env,
                wait_health=True,
                stdout=log_file,
                stderr=subprocess.STDOUT,
            ):
                list_result = subprocess.run(
                    [
                        self.cli_bin,
                        "--host",
                        "127.0.0.1",
                        "--port",
                        str(self.port),
                        "list",
                    ],
                    capture_output=True,
                    text=True,
                    env=env,
                )
                self.assertEqual(list_result.returncode, 0, list_result.stderr)
                component_rows = [
                    line for line in list_result.stdout.splitlines() if comp_id in line
                ]
                self.assertTrue(component_rows, list_result.stdout)
                self.assertIn("No", component_rows[0])

                subprocess.run(
                    [
                        self.cli_bin,
                        "--host",
                        "127.0.0.1",
                        "--port",
                        str(self.port),
                        "pull",
                        coll_id,
                    ],
                    capture_output=True,
                    text=True,
                    env=env,
                )

        with open(log_path, "r", encoding="utf-8", errors="replace") as f:
            log_output = f.read()
        self.assertIn(f"Downloading component: {comp_id}", log_output)


if __name__ == "__main__":
    unittest.main()

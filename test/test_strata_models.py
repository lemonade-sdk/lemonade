import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "strata_prepare", ROOT / "src" / "cpp" / "resources" / "strata_prepare.py"
)
bridge = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bridge)


class StrataModelsTest(unittest.TestCase):
    def test_preparation(self):
        setup = SimpleNamespace(
            FAMILIES={"qwen": {}},
            OWN_PATH_FAMILIES={"qwen36": {"arch": "qwen35moe"}},
            get_llama_cpp=lambda: Path("llama.cpp"),
            ram_gb=lambda: 64,
            kv_streaming_wanted=lambda *_: True,
            model_shards=lambda *_: 2,
        )
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            request = {
                "backend": "rocm",
                "context": 262144,
                "parallel": 1,
                "paths": {
                    "main": str(root / "model.gguf"),
                    "shard_2": str(root / "model-2.gguf"),
                },
                "output": str(root),
                "cache": str(root / "cache"),
                "mtp_cache": str(root / "mtp"),
                "offline": False,
                "executable": str(root / "strata"),
                "name": "model",
                "args": ["--chunk", "512"],
                "config": str(root / "config.json"),
            }
            for family, model, tool in (
                ("qwen36", "Q8_0", "tools/strata_tokenizer.py"),
                ("qwen", "Q2_0", "tools/iq_pack.py"),
            ):
                with self.subTest(family=family), patch.object(
                    bridge, "run_tool"
                ) as run, patch.object(
                    bridge, "prepare_mtp", return_value=root / "mtp" / "rt"
                ), patch.object(
                    bridge, "ple_shard", return_value=root / "model.gguf"
                ):
                    request["selection"] = {
                        "family": family,
                        "model": model,
                        "backends": ["rocm"],
                    }
                    bridge.prepare(setup, root, request)
                    config = json.loads(Path(request["config"]).read_text())
                    self.assertEqual(run.call_args.args[1], tool)
                    args = config["args"]
                    self.assertEqual(args[args.index("--max-context") + 1], "262144")
                    self.assertEqual(config["user_args"], ["--chunk", "512"])
                    self.assertNotIn("--prefill", args)
                    self.assertEqual(config["backend"], "hip")
                    self.assertEqual(
                        args[args.index("--mtp") + 1],
                        "2" if family == "qwen36" else str(root / "mtp" / "rt"),
                    )

    def test_catalog(self):
        registry = json.loads(
            (ROOT / "src" / "cpp" / "resources" / "server_models.json").read_text()
        )
        models = [row for row in registry.values() if row.get("recipe") == "strata"]
        self.assertEqual(len(models), 27)
        for row in models:
            self.assertNotIn("DeepSeek", str(row))
            self.assertNotIn("NVFP4-QAD", str(row))
            sources = row.get("checkpoints", {"main": row.get("checkpoint")})
            revisions = row["strata"].get("revisions")
            if revisions is not None:
                self.assertEqual(set(sources), set(revisions))
            else:
                self.assertEqual(len(row["strata"]["revision"]), 40)


if __name__ == "__main__":
    unittest.main()

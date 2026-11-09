import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "strata_models", ROOT / "src" / "cpp" / "resources" / "strata_models.py"
)
bridge = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bridge)


class StrataModelsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.setup = SimpleNamespace(
            FAMILIES={
                "qwen": {"file": "model-{i}.gguf"},
                "unsloth": {"pack_args": ["--compat-bf16"]},
                "nvfp4": {"ready": True},
            },
            MODELS={"Q2_0": {}, "UD-IQ1_M": {}, "NVFP4": {"repo": "repo/pack"}},
            OWN_PATH_FAMILIES={"qwen36": {"arch": "qwen35moe"}},
            get_llama_cpp=lambda: self.root / "llama.cpp",
            ram_gb=lambda: 64,
            kv_streaming_wanted=Mock(return_value=False),
            model_shards=lambda _definition, _model: 2,
            model_file=lambda _definition, _model, i: f"model-{i}.gguf",
        )
        self.ple = self.root / "model-2.gguf"
        self.reader = Mock(
            side_effect=lambda path: SimpleNamespace(
                tensors=(
                    [SimpleNamespace(name="per_layer_token_embd.weight")]
                    if path == self.ple
                    else []
                )
            )
        )
        self.request = {
            "selection": {"family": "qwen36", "model": "Q8_0", "backends": ["rocm"]},
            "backend": "rocm",
            "context": 262144,
            "parallel": 1,
            "paths": {
                "main": str(self.root / "model.gguf"),
                "shard_2": str(self.root / "model-2.gguf"),
            },
            "output": str(self.root / "runtime"),
            "cache": str(self.root / "cache"),
            "mtp_cache": str(self.root / "shared-mtp"),
            "offline": False,
            "executable": str(self.root / "strata"),
            "name": "model",
            "args": [],
            "config": str(self.root / "config.json"),
        }

    def prepare(self):
        with patch.object(bridge, "run_tool") as run, patch.object(
            bridge, "prepare_mtp", return_value=self.root / "shared-mtp" / "rt"
        ) as mtp, patch.dict(
            sys.modules, {"gguf_reader": SimpleNamespace(GGUFFile=self.reader)}
        ):
            bridge.prepare(self.setup, self.root, self.request)
        self.mtp_preparation = mtp
        return json.loads(Path(self.request["config"]).read_text()), run

    def test_qwen36_preserves_context_and_default_mtp(self):
        config, run = self.prepare()
        self.assertEqual(config["arch"], "qwen35moe")
        self.assertEqual(config["backend"], "hip")
        self.assertEqual(
            config["args"],
            [
                "--native",
                self.request["paths"]["main"],
                "--max-context",
                "262144",
                "--prefill",
                "auto",
                "--mtp",
                "2",
            ],
        )
        self.assertEqual(run.call_args.args[1], "tools/strata_tokenizer.py")

    def test_explicit_mtp_and_chunk_override_defaults(self):
        self.request["args"] = ["--mtp=1", "--chunk", "512"]
        config, _ = self.prepare()
        self.assertNotIn("--mtp", config["args"])
        self.assertNotIn("--prefill", config["args"])
        self.assertEqual(config["args"][-3:], ["--mtp=1", "--chunk", "512"])

    def test_flash_prepares_without_copying_experts(self):
        self.request["selection"] = {
            "family": "qwen",
            "model": "Q2_0",
            "backends": ["rocm"],
        }
        config, run = self.prepare()
        self.assertEqual(run.call_args.args[1], "tools/iq_pack.py")
        self.assertNotIn("--copy-experts", run.call_args.args)
        self.assertIn("--ple-gguf", config["args"])
        self.assertIn("--spec", config["args"])
        self.assertEqual(
            config["args"][config["args"].index("--mtp") + 1],
            str(self.root / "shared-mtp" / "rt"),
        )
        self.mtp_preparation.assert_called_once_with(
            self.setup, self.root, Path(self.request["mtp_cache"]), offline=False
        )

    def test_disabling_speculation_skips_mtp_download_and_configuration(self):
        self.request["selection"] = {
            "family": "qwen",
            "model": "Q2_0",
            "backends": ["rocm"],
        }
        for args in (["--spec", "0"], ["--spec=0"]):
            with self.subTest(args=args):
                self.request["args"] = args
                config, _ = self.prepare()
                self.mtp_preparation.assert_not_called()
                self.assertNotIn("--mtp", config["args"])

    def test_custom_mtp_pack_skips_default_download(self):
        self.request["selection"] = {
            "family": "qwen",
            "model": "Q2_0",
            "backends": ["rocm"],
        }
        self.request["args"] = ["--mtp", "custom-pack"]
        config, _ = self.prepare()
        self.mtp_preparation.assert_not_called()
        self.assertEqual(config["args"].count("--mtp"), 1)
        self.assertEqual(config["args"][-2:], ["--mtp", "custom-pack"])

    def test_long_context_uses_upstream_ram_guard_for_kv_streaming(self):
        self.request["selection"] = {
            "family": "qwen",
            "model": "Q2_0",
            "backends": ["rocm"],
        }
        for enabled in (False, True):
            with self.subTest(enabled=enabled):
                self.setup.kv_streaming_wanted.return_value = enabled
                config, _ = self.prepare()
                self.assertEqual("--kv-resident" in config["args"], enabled)
                self.setup.kv_streaming_wanted.assert_called_with(
                    "Q2_0", 262144, "int8", 64
                )
                if enabled:
                    self.assertEqual(
                        config["args"][config["args"].index("--kv-resident") + 1],
                        "32768",
                    )

    def test_kv_streaming_honors_precision_and_residency_overrides(self):
        self.request["selection"] = {
            "family": "qwen",
            "model": "Q2_0",
            "backends": ["rocm"],
        }
        self.setup.kv_streaming_wanted.return_value = True
        self.request["args"] = ["--kv", "fp16", "--kv-resident", "0"]
        config, _ = self.prepare()
        self.setup.kv_streaming_wanted.assert_called_once_with(
            "Q2_0", 262144, "fp16", 64
        )
        self.assertEqual(config["args"].count("--kv"), 1)
        self.assertEqual(config["args"].count("--kv-resident"), 1)
        self.assertEqual(config["args"][-4:], ["--kv", "fp16", "--kv-resident", "0"])

    def test_explicit_mtp_off_preserves_other_speculation_settings(self):
        self.request["selection"] = {
            "family": "qwen",
            "model": "Q2_0",
            "backends": ["rocm"],
        }
        for args in (["--mtp", "off"], ["--mtp=off"]):
            with self.subTest(args=args):
                self.request["args"] = [*args, "--spec", "4"]
                config, _ = self.prepare()
                self.mtp_preparation.assert_not_called()
                self.assertNotIn("--mtp", config["args"])
                self.assertEqual(config["args"][-2:], ["--spec", "4"])

    def test_mtp_preparation_is_shared_and_reused(self):
        cache = Path(self.request["mtp_cache"])
        fetch = Mock()
        self.setup.fetch_mtp = fetch

        def finish(runtime):
            for name in ("dense.bin", "dense.txt", "experts.bin", "draft_vocab.bin"):
                (runtime / name).write_bytes(b"prepared")

        self.setup.refresh_draft_vocab = finish

        def tool(_control, name, *args):
            if name == "tools/mtp_rt.py":
                (cache / "rt").mkdir(parents=True)

        with patch.object(bridge, "run_tool", side_effect=tool) as run:
            runtime = bridge.prepare_mtp(self.setup, self.root, cache)
            self.assertEqual(
                bridge.prepare_mtp(self.setup, self.root, cache, offline=True), runtime
            )
        fetch.assert_called_once()
        self.assertEqual(
            [call.args[1] for call in run.call_args_list],
            ["tools/mtp_pack.py", "tools/mtp_rt.py"],
        )
        self.assertTrue((cache / ".lemonade-ready").is_file())

    def test_failed_mtp_preparation_is_not_marked_ready(self):
        cache = Path(self.request["mtp_cache"])
        self.setup.fetch_mtp = Mock(side_effect=RuntimeError("download failed"))
        with self.assertRaisesRegex(RuntimeError, "download failed"):
            bridge.prepare_mtp(self.setup, self.root, cache)
        self.assertFalse((cache / ".lemonade-ready").exists())

    def test_missing_offline_mtp_reports_error_without_downloading(self):
        cache = Path(self.request["mtp_cache"])
        self.setup.fetch_mtp = Mock()
        with self.assertRaisesRegex(RuntimeError, "while offline"):
            bridge.prepare_mtp(self.setup, self.root, cache, offline=True)
        self.setup.fetch_mtp.assert_not_called()
        self.assertFalse((cache / ".lemonade-ready").exists())

    def test_incomplete_mtp_tools_output_is_not_marked_ready(self):
        cache = Path(self.request["mtp_cache"])
        self.setup.fetch_mtp = Mock()
        self.setup.refresh_draft_vocab = Mock()
        with patch.object(bridge, "run_tool"):
            with self.assertRaisesRegex(RuntimeError, "all runtime files"):
                bridge.prepare_mtp(self.setup, self.root, cache)
        self.assertFalse((cache / ".lemonade-ready").exists())

    def test_unsloth_uses_upstream_compatibility_argument(self):
        self.request["selection"] = {
            "family": "unsloth",
            "model": "UD-IQ1_M",
            "backends": ["rocm"],
        }
        config, run = self.prepare()
        self.assertIn("--compat-bf16", run.call_args.args)
        self.assertEqual(
            config["args"][config["args"].index("--ple-gguf") + 1], str(self.ple)
        )

    def test_ple_selection_reads_headers_instead_of_assuming_the_last_shard(self):
        self.request["selection"] = {
            "family": "qwen",
            "model": "Q2_0",
            "backends": ["rocm"],
        }
        for role in ("main", "shard_2"):
            with self.subTest(role=role):
                self.ple = Path(self.request["paths"][role])
                config, _ = self.prepare()
                self.assertEqual(
                    config["args"][config["args"].index("--ple-gguf") + 1],
                    str(self.ple),
                )

    def test_many_shards_use_upstream_native_shard_discovery(self):
        self.setup.model_shards = lambda _definition, _model: 4
        self.request["selection"] = {
            "family": "qwen",
            "model": "Q2_0",
            "backends": ["rocm"],
        }
        self.request["paths"].update(
            shard_3=str(self.root / "model-3.gguf"),
            shard_4=str(self.root / "model-4.gguf"),
        )
        self.ple = Path(self.request["paths"]["shard_4"])
        config, _ = self.prepare()
        self.assertNotIn("--ple-gguf", config["args"])
        self.reader.assert_any_call(self.ple)

    def test_missing_ple_tensor_reports_a_preparation_error(self):
        self.request["selection"] = {
            "family": "qwen",
            "model": "Q2_0",
            "backends": ["rocm"],
        }
        self.ple = self.root / "missing.gguf"
        with self.assertRaisesRegex(ValueError, "No per_layer_token_embd.weight"):
            self.prepare()
        self.assertFalse(Path(self.request["config"]).exists())

    def test_failed_preparation_is_retried(self):
        with patch.object(
            bridge, "run_tool", side_effect=subprocess.CalledProcessError(1, "pack")
        ):
            with self.assertRaises(subprocess.CalledProcessError):
                bridge.prepare(self.setup, self.root, self.request)
        self.assertFalse((Path(self.request["cache"]) / ".lemonade-ready").exists())

    def test_ready_pack_reuses_shared_auxiliary_files(self):
        self.request.update(backend="cuda")
        self.request["selection"] = {
            "family": "nvfp4",
            "model": "NVFP4",
            "backends": ["cuda"],
        }
        self.request["paths"].update(
            dense=str(self.root / "dense.gguf"),
            ple="shared/ple.gguf",
            embd="shared/embd.gguf",
        )
        config, run = self.prepare()
        run.assert_not_called()
        self.assertIn(str(Path("shared") / "ple.gguf"), config["args"])
        self.assertIn(str(Path("shared") / "embd.gguf"), config["args"])

    def test_deepseek_and_incompatible_targets_are_rejected(self):
        self.request["selection"]["family"] = "deepseek41"
        with self.assertRaisesRegex(ValueError, "Unsupported Qwen engine family"):
            self.prepare()
        self.request["selection"]["family"] = "qwen36"
        self.request["backend"] = "metal"
        with self.assertRaisesRegex(ValueError, "does not support metal"):
            self.prepare()

    def test_metal_uses_upstream_repacker_and_streaming_defaults(self):
        self.request.update(backend="metal")
        self.request["selection"] = {
            "family": "qwen",
            "model": "Q2_0",
            "backends": ["metal"],
        }
        config, run = self.prepare()
        self.assertEqual(run.call_args_list[0].args[1], "metal/tools/metal_repack.py")
        self.assertEqual(run.call_args_list[1].args[1], "tools/iq_pack.py")
        self.assertEqual(config["env"]["STRATA_METAL_STREAM"], "1")
        self.assertEqual(config["args"][config["args"].index("--spec") + 1], "2")
        self.assertNotIn("--kv", config["args"])
        self.assertNotIn("--mtp", config["args"])
        self.mtp_preparation.assert_not_called()

    def test_intel_uses_the_driver_specific_upstream_policy(self):
        self.request.update(backend="sycl")
        self.request["selection"] = {
            "family": "qwen",
            "model": "Q2_0",
            "backends": ["sycl"],
        }
        for driver, expected in (("i915", "--ple-io"), ("xe", "--stream-experts")):
            with self.subTest(driver=driver):
                with patch.dict(
                    sys.modules,
                    {
                        "strata_cli.launch.adopt": SimpleNamespace(
                            intel_driver=lambda: driver
                        )
                    },
                ):
                    config, _ = self.prepare()
                self.assertIn(expected, config["args"])
                self.assertIn("--vram-reserve-mib", config["args"])

    def test_checkpoint_retains_hub_subdirectory(self):
        self.assertEqual(
            bridge.checkpoint(
                "https://huggingface.co/owner/repo/resolve/main/Q2_0/", "model.gguf"
            ),
            "owner/repo:Q2_0/model.gguf",
        )

    def test_catalog_update_preserves_unrelated_formatting(self):
        original = '{\n    "other": {"recipe":"other", "labels": ["chat"]}\n}\n'
        rows = {"Qwen-Strata": {"recipe": "strata", "size": 1}}
        updated = bridge.update_catalog(original, rows)
        self.assertIn('"other": {"recipe":"other", "labels": ["chat"]}', updated)
        self.assertEqual(json.loads(updated)["Qwen-Strata"], rows["Qwen-Strata"])
        self.assertEqual(bridge.update_catalog(updated, rows), updated)

    def test_committed_catalog_is_qwen_engine_only_and_complete(self):
        registry = json.loads(
            (ROOT / "src" / "cpp" / "resources" / "server_models.json").read_text()
        )
        models = [row for row in registry.values() if row.get("recipe") == "strata"]
        self.assertEqual(len(models), 27)
        self.assertEqual(
            {row["strata"]["family"] for row in models},
            {"qwen", "qwen36", "swift", "coder", "unsloth", "gyro", "nvfp4"},
        )
        for row in models:
            self.assertNotIn("DeepSeek", str(row))
            self.assertNotIn("NVFP4-QAD", str(row))
            sources = row.get("checkpoints", {"main": row.get("checkpoint")})
            if "-00001-of-" in sources["main"]:
                count = int(sources["main"].split("-of-")[1].removesuffix(".gguf"))
                self.assertEqual(len(sources), count)
            self.assertEqual(set(sources), set(row["strata"]["revisions"]))
        packs = [row for row in models if row["strata"]["format"] == "ready"]
        self.assertEqual(len(packs), 2)
        self.assertEqual(packs[0]["checkpoints"]["ple"], packs[1]["checkpoints"]["ple"])
        self.assertEqual(
            packs[0]["checkpoints"]["embd"], packs[1]["checkpoints"]["embd"]
        )


if __name__ == "__main__":
    unittest.main()

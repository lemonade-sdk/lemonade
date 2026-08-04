#!/usr/bin/env python3
"""
CPU-runnable unit tests for .github/scripts/select_inference_engines.py,
which decides which inference-test matrix legs run for a changed-file set.

Run with: python -m pytest test/test_inference_engine_selection.py
      or: python test/test_inference_engine_selection.py
"""

import importlib.util
import json
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SCRIPT_PATH = REPO_ROOT / ".github" / "scripts" / "select_inference_engines.py"
MATRIX_PATH = REPO_ROOT / ".github" / "inference-matrix.json"

spec = importlib.util.spec_from_file_location("select_inference_engines", SCRIPT_PATH)
sel = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sel)

with open(MATRIX_PATH, encoding="utf-8") as f:
    MATRIX = json.load(f)

ALL_ENGINE_NAMES = {leg["name"] for legs in MATRIX.values() for leg in legs}


def outputs_for(changed_files, event_name="pull_request"):
    selection = sel.select_engines(changed_files, event_name)
    return sel.build_outputs(selection, MATRIX)


def selected_names(outputs, key):
    return {leg["name"] for leg in json.loads(outputs[key])}


class TestSafeFiles(unittest.TestCase):
    def test_docs_and_ui_select_nothing(self):
        for path in [
            "docs/dev/job-system.md",
            "README.md",
            "mkdocs.yml",
            "examples/demo.py",
            "src/app/src/renderer/ChatWindow.tsx",
            "src/web-app/package.json",
            "LICENSE",
            ".github/workflows/launchpad-ppa.yml",
            ".github/runners.json",
        ]:
            outputs = outputs_for([path])
            self.assertEqual(outputs["run_exe"], "false", path)
            self.assertEqual(outputs["run_deb"], "false", path)
            self.assertEqual(outputs["run_dmg"], "false", path)
            self.assertEqual(outputs["exe_matrix"], "[]", path)
            self.assertEqual(outputs["selected"], "none", path)


class TestEngineFiles(unittest.TestCase):
    def test_kokoro_selects_only_tts(self):
        outputs = outputs_for(["src/cpp/server/backends/kokoro/foo.cpp"])
        self.assertEqual(selected_names(outputs, "exe_matrix"), {"text-to-speech"})
        self.assertEqual(selected_names(outputs, "deb_matrix"), {"text-to-speech"})
        self.assertEqual(outputs["run_dmg"], "true")

    def test_llamacpp_selects_llamacpp_and_omni(self):
        outputs = outputs_for(["src/cpp/server/backends/llamacpp/x.cpp"])
        self.assertEqual(selected_names(outputs, "exe_matrix"), {"llamacpp", "omni"})
        self.assertEqual(selected_names(outputs, "deb_matrix"), {"llamacpp", "omni"})
        self.assertEqual(outputs["run_dmg"], "true")

    def test_exe_only_engine_skips_deb(self):
        outputs = outputs_for(["src/cpp/server/backends/ryzenai/x.cpp"])
        self.assertEqual(selected_names(outputs, "exe_matrix"), {"ryzenai"})
        self.assertEqual(outputs["run_exe"], "true")
        self.assertEqual(outputs["run_deb"], "false")
        self.assertEqual(outputs["run_dmg"], "false")

    def test_cloud_backend_selects_router_leg(self):
        outputs = outputs_for(["src/cpp/server/backends/cloud/cloud_server.cpp"])
        self.assertEqual(selected_names(outputs, "exe_matrix"), {"router-onnxruntime"})

    def test_vllm_backend_selects_nothing(self):
        outputs = outputs_for(["src/cpp/server/backends/vllm/x.cpp"])
        self.assertEqual(outputs["run_exe"], "false")
        self.assertEqual(outputs["run_deb"], "false")

    def test_engine_test_script_selects_its_engines(self):
        outputs = outputs_for(["test/server_whisper.py"])
        self.assertEqual(
            selected_names(outputs, "exe_matrix"), {"whisper", "flm-whisper"}
        )
        self.assertEqual(selected_names(outputs, "deb_matrix"), {"whisper"})


class TestRunAllFallback(unittest.TestCase):
    def assert_all(self, outputs):
        self.assertEqual(
            selected_names(outputs, "exe_matrix"),
            {leg["name"] for leg in MATRIX["exe"]},
        )
        self.assertEqual(
            selected_names(outputs, "deb_matrix"),
            {leg["name"] for leg in MATRIX["deb"]},
        )
        self.assertEqual(outputs["run_dmg"], "true")

    def test_core_and_infra_files_select_all(self):
        for path in [
            "src/cpp/server/router.cpp",
            "src/cpp/server/server.cpp",
            "src/cpp/include/lemon/wrapped_server.h",
            "src/cpp/resources/server_models.json",
            "src/cpp/server/backends/backend_registry.cpp",
            "src/cpp/server/backends/unknown_new_backend/x.cpp",
            "CMakeLists.txt",
            "test/utils/server_base.py",
            "test/requirements.txt",
            ".github/workflows/cpp_server_build_test_release.yml",
            ".github/actions/setup-venv/action.yml",
            ".github/inference-matrix.json",
            ".github/scripts/select_inference_engines.py",
            "some/new/unknown_path.txt",
        ]:
            self.assert_all(outputs_for([path]))

    def test_non_pull_request_events_select_all(self):
        for event in ["push", "merge_group", "workflow_dispatch"]:
            self.assert_all(outputs_for(["docs/index.md"], event_name=event))

    def test_empty_change_list_selects_all(self):
        self.assert_all(outputs_for([]))


class TestMixedChanges(unittest.TestCase):
    def test_safe_plus_engine_selects_just_engine(self):
        outputs = outputs_for(
            ["docs/guide/cli.md", "src/cpp/server/backends/trellis/x.cpp"]
        )
        self.assertEqual(selected_names(outputs, "exe_matrix"), {"3d-trellis"})

    def test_safe_plus_unknown_selects_all(self):
        outputs = outputs_for(["docs/guide/cli.md", "tools/new_tool.py"])
        self.assertEqual(selected_names(outputs, "exe_matrix"), ALL_ENGINE_NAMES)


class TestMappingIntegrity(unittest.TestCase):
    def test_every_matrix_leg_is_reachable_by_some_rule(self):
        mapped = set()
        for engines in sel.BACKEND_DIR_TO_ENGINES.values():
            mapped.update(engines)
        for engines in sel.TEST_SCRIPT_TO_ENGINES.values():
            mapped.update(engines)
        self.assertEqual(ALL_ENGINE_NAMES - mapped, set())

    def test_every_mapping_target_exists_in_matrix(self):
        for source in (sel.BACKEND_DIR_TO_ENGINES, sel.TEST_SCRIPT_TO_ENGINES):
            for key, engines in source.items():
                for engine in engines:
                    self.assertIn(engine, ALL_ENGINE_NAMES, f"{key} -> {engine}")

    def test_every_backend_folder_has_a_rule(self):
        backends_dir = REPO_ROOT / "src" / "cpp" / "server" / "backends"
        folders = {p.name for p in backends_dir.iterdir() if p.is_dir()}
        self.assertEqual(folders - set(sel.BACKEND_DIR_TO_ENGINES), set())

    def test_dmg_engines_exist_in_matrix(self):
        self.assertEqual(sel.DMG_ENGINES - ALL_ENGINE_NAMES, set())

    def test_matrix_legs_keep_required_fields(self):
        for legs in MATRIX.values():
            for leg in legs:
                self.assertEqual(
                    sorted(leg), ["backends", "extra_args", "name", "runner", "script"]
                )
                self.assertIsInstance(leg["runner"], list)


if __name__ == "__main__":
    unittest.main()

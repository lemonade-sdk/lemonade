#!/usr/bin/env python3
"""
CPU-runnable unit tests for .github/scripts/select_inference_engines.py,
which decides which inference-test matrix legs run for a changed-file set.

Run with: python -m pytest test/test_inference_engine_selection.py
      or: python test/test_inference_engine_selection.py
"""

import copy
import importlib.util
import json
import re
import unittest
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parent.parent
SCRIPT_PATH = REPO_ROOT / ".github" / "scripts" / "select_inference_engines.py"
MATRIX_PATH = REPO_ROOT / ".github" / "inference-matrix.json"

spec = importlib.util.spec_from_file_location("select_inference_engines", SCRIPT_PATH)
sel = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sel)

WORKFLOW_PATH = REPO_ROOT / sel.WORKFLOW_FILE

with open(MATRIX_PATH, encoding="utf-8") as f:
    MATRIX = json.load(f)

ALL_ENGINE_NAMES = {leg["name"] for legs in MATRIX.values() for leg in legs}
SCRIPT_RULES = sel.derive_script_rules(MATRIX)


def outputs_for(changed_files, event_name="pull_request"):
    selection = sel.select_engines(changed_files, event_name, SCRIPT_RULES)
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
            ".github/workflows/routing_schema_tests.yml",
            ".github/workflows/mlc_config.json",
            ".github/runners.json",
            ".github/CODEOWNERS",
            ".github/dependabot.yml",
            ".github/ISSUE_TEMPLATE/bug-report.yml",
            ".pre-commit-config.yaml",
            "src/cpp/tray/tray_app.cpp",
            "test/app/appSettings.test.cjs",
            "test/cpp/test_auto_tune.cpp",
            "test/server_endpoints.py",
            "test/server_cli2.py",
            "test/test_tray_https.py",
        ]:
            outputs = outputs_for([path])
            self.assertEqual(outputs["run_exe"], "false", path)
            self.assertEqual(outputs["run_deb"], "false", path)
            self.assertEqual(outputs["run_dmg"], "false", path)
            self.assertEqual(outputs["exe_matrix"], "[]", path)
            self.assertEqual(outputs["selected"], "none", path)


class TestEngineFiles(unittest.TestCase):
    def test_kokoro_selects_tts_and_the_omni_collection(self):
        outputs = outputs_for(["src/cpp/server/backends/kokoro/foo.cpp"])
        self.assertEqual(
            selected_names(outputs, "exe_matrix"), {"text-to-speech", "omni"}
        )
        self.assertEqual(
            selected_names(outputs, "deb_matrix"), {"text-to-speech", "omni"}
        )
        self.assertEqual(outputs["run_dmg"], "true")

    def test_sdcpp_selects_stable_diffusion_and_the_omni_collection(self):
        outputs = outputs_for(["src/cpp/server/backends/sdcpp/sd_server.cpp"])
        self.assertEqual(
            selected_names(outputs, "exe_matrix"), {"stable-diffusion", "omni"}
        )

    def test_llamacpp_selects_its_legs_and_the_router(self):
        outputs = outputs_for(["src/cpp/server/backends/llamacpp/x.cpp"])
        self.assertEqual(
            selected_names(outputs, "exe_matrix"),
            {"llamacpp", "omni", "router-onnxruntime"},
        )
        self.assertEqual(selected_names(outputs, "deb_matrix"), {"llamacpp", "omni"})
        self.assertEqual(outputs["run_dmg"], "true")

    def test_exe_only_engine_skips_deb(self):
        outputs = outputs_for(["src/cpp/server/backends/ryzenai/x.cpp"])
        self.assertEqual(selected_names(outputs, "exe_matrix"), {"ryzenai"})
        self.assertEqual(outputs["run_exe"], "true")
        self.assertEqual(outputs["run_deb"], "false")
        self.assertEqual(outputs["run_dmg"], "false")

    def test_backend_headers_narrow_like_backend_sources(self):
        outputs = outputs_for(["src/cpp/include/lemon/backends/trellis/trellis.h"])
        self.assertEqual(selected_names(outputs, "exe_matrix"), {"3d-trellis"})

    def test_loose_file_in_either_backend_dir_selects_all(self):
        for path in [
            "src/cpp/server/backends/backend_registry.cpp",
            "src/cpp/include/lemon/backends/backend_registry.h",
        ]:
            self.assertEqual(
                selected_names(outputs_for([path]), "exe_matrix"), ALL_ENGINE_NAMES
            )

    def test_cloud_backend_selects_router_leg(self):
        outputs = outputs_for(["src/cpp/server/backends/cloud/cloud_server.cpp"])
        self.assertEqual(selected_names(outputs, "exe_matrix"), {"router-onnxruntime"})

    def test_vllm_backend_selects_nothing(self):
        outputs = outputs_for(["src/cpp/server/backends/vllm/x.cpp"])
        self.assertEqual(outputs["run_exe"], "false")
        self.assertEqual(outputs["run_deb"], "false")

    def test_whispercpp_selects_whisper_and_the_omni_collection(self):
        outputs = outputs_for(["src/cpp/server/backends/whispercpp/x.cpp"])
        self.assertEqual(selected_names(outputs, "exe_matrix"), {"whisper", "omni"})

    def test_a_non_leg_script_that_becomes_a_leg_stops_being_safe(self):
        self.assertEqual(outputs_for(["test/server_endpoints.py"])["selected"], "none")
        extended = copy.deepcopy(MATRIX)
        extended["exe"].append(
            dict(extended["exe"][0], name="endpoints", script="server_endpoints.py")
        )
        rules = sel.derive_script_rules(extended)
        self.assertEqual(
            sel.classify_file("test/server_endpoints.py", rules), {"endpoints"}
        )

    def test_shared_test_helpers_still_select_all(self):
        for path in ["test/utils/server_base.py", "test/requirements.txt"]:
            self.assertEqual(
                selected_names(outputs_for([path]), "exe_matrix"), ALL_ENGINE_NAMES
            )

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
            ".github/some_new_thing.yml",
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
    def test_rules_and_matrix_agree(self):
        sel.validate_mapping(MATRIX)

    def test_validate_mapping_catches_a_renamed_leg(self):
        broken = copy.deepcopy(MATRIX)
        broken["exe"][0]["name"] = "renamed-leg"
        with self.assertRaises(SystemExit):
            sel.validate_mapping(broken)

    def test_validate_mapping_catches_a_leg_name_that_breaks_the_output(self):
        broken = copy.deepcopy(MATRIX)
        broken["exe"][0]["name"] = "llamacpp\nrun_exe=false"
        with self.assertRaises(SystemExit):
            sel.validate_mapping(broken)

    def test_validate_mapping_catches_an_unreachable_leg(self):
        broken = copy.deepcopy(MATRIX)
        broken["exe"].append(dict(broken["exe"][0], name="brand-new-leg"))
        with self.assertRaises(SystemExit):
            sel.validate_mapping(broken)

    def test_a_new_leg_is_wired_to_its_test_script_automatically(self):
        extended = copy.deepcopy(MATRIX)
        extended["exe"].append(dict(extended["exe"][0], name="llamacpp-rocm"))
        rules = sel.derive_script_rules(extended)
        self.assertIn("llamacpp-rocm", rules["test/server_llm.py"])

    def test_workflows_that_touch_inference_are_listed(self):
        markers = ("inference-matrix.json", "test_inference_engine_selection.py")
        found = set()
        for path in sorted((REPO_ROOT / ".github" / "workflows").glob("*.y*ml")):
            text = path.read_text(encoding="utf-8")
            if any(marker in text for marker in markers):
                found.add(f".github/workflows/{path.name}")
        self.assertEqual(found, set(sel.INFERENCE_WORKFLOWS))

    def test_every_backend_folder_has_a_rule(self):
        for prefix in sel.BACKEND_DIRS:
            folders = {p.name for p in (REPO_ROOT / prefix).iterdir() if p.is_dir()}
            self.assertTrue(folders, prefix)
            self.assertEqual(folders - set(sel.BACKEND_DIR_TO_ENGINES), set(), prefix)

    def test_every_gated_job_is_covered_by_the_aggregate(self):
        with open(WORKFLOW_PATH, encoding="utf-8") as f:
            jobs = yaml.safe_load(f)["jobs"]
        covered = set()
        for name, job in jobs.items():
            if "needs.changes.outputs" not in str(job.get("if", "")):
                continue
            needs = job.get("needs") or []
            covered.add(name)
            covered.update([needs] if isinstance(needs, str) else needs)
        self.assertTrue(covered, "no change-detection-gated jobs found")
        self.assertEqual(covered - set(jobs["inference-tests-ok"]["needs"]), set())

    def test_dmg_engines_exist_in_matrix(self):
        self.assertEqual(sel.DMG_ENGINES - ALL_ENGINE_NAMES, set())

    def test_dmg_engines_match_the_dmg_job(self):
        """DMG_ENGINES is hand-maintained; tie it to what the job really runs."""
        with open(WORKFLOW_PATH, encoding="utf-8") as f:
            workflow = yaml.safe_load(f)
        steps = workflow["jobs"]["test-dmg-inference"]["steps"]
        scripts = {
            f"test/{name}"
            for step in steps
            for name in re.findall(r"test/(server_\w+\.py)", step.get("run", ""))
        }
        self.assertTrue(scripts, "no test scripts found in test-dmg-inference")
        reachable = set()
        for script in sorted(scripts):
            self.assertIn(script, SCRIPT_RULES)
            engines = SCRIPT_RULES[script]
            self.assertTrue(
                engines & sel.DMG_ENGINES,
                f"{script} runs on .dmg but none of {sorted(engines)} is in DMG_ENGINES",
            )
            reachable |= engines
        self.assertEqual(
            sel.DMG_ENGINES - reachable,
            set(),
            "DMG_ENGINES lists engines the .dmg job never runs",
        )

    def test_matrix_legs_keep_required_fields(self):
        for legs in MATRIX.values():
            for leg in legs:
                self.assertEqual(
                    sorted(leg), ["backends", "extra_args", "name", "runner", "script"]
                )
                self.assertIsInstance(leg["runner"], list)


if __name__ == "__main__":
    unittest.main()

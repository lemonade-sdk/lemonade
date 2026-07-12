"""
Verifies the configuration entries (JSON/YAML/Python) added for the VTE
backend: server_models.json, backend_versions.json, test/utils/capabilities.py,
and the new CI matrix row. Does not replace running the real test suite
(test/server_llm.py --wrapped-server vte --backend rocm) against a compiled
lemond -- that still depends on cmake.

Delete this folder once the integration is complete and validated.
"""
import json
import sys
from pathlib import Path

import pytest
import yaml

REPO = Path(__file__).resolve().parent.parent
SERVER_MODELS = REPO / "src/cpp/resources/server_models.json"
BACKEND_VERSIONS = REPO / "src/cpp/resources/backend_versions.json"
CAPABILITIES_PY = REPO / "test/utils/capabilities.py"
CI_WORKFLOW = REPO / ".github/workflows/cpp_server_build_test_release.yml"


def test_server_models_json_valid_and_has_vte_entry():
    data = json.loads(SERVER_MODELS.read_text(encoding="utf-8"))
    assert "Qwen2.5-1.5B-Instruct-VTE" in data
    entry = data["Qwen2.5-1.5B-Instruct-VTE"]
    assert entry["recipe"] == "vte"
    assert entry["checkpoint"] == "Qwen/Qwen2.5-1.5B-Instruct-GGUF:qwen2.5-1.5b-instruct-q4_k_m.gguf"
    # suggested=True is what makes a model show up in Model Manager's browse/download
    # list at all (ModelManager.tsx's suggestedModels filters on `info.suggested`) --
    # NOT an endorsement that VTE is production-ready. The backend's own
    # `experimental: true` flag (VTE.h) is what marks it opt-in; "suggested" is a
    # visibility switch, a distinction found the hard way: Granite/Qwen3.5-VTE were
    # unreachable from the UI with suggested=False, even though they were fully
    # registered and downloadable via the API.
    assert entry["suggested"] is True


def test_server_models_json_has_granite_and_qwen35_vte_entries():
    """Granite (Q8_0) and Qwen3.5 (Q6_K) are the other two architectures VTE
    knows how to run (see vte/compiler/sanitizer.py::SUPPORTED_ARCHITECTURES).
    Both checkpoints were downloaded and loaded for real (VTEModel.
    from_pretrained + a real generate() call, not just read from the Hub)
    before entering here, confirming each quantization works on VTE."""
    data = json.loads(SERVER_MODELS.read_text(encoding="utf-8"))

    granite = data["Granite-4.1-3B-VTE"]
    assert granite["recipe"] == "vte"
    assert granite["checkpoint"] == "unsloth/granite-4.1-3b-GGUF:granite-4.1-3b-Q8_0.gguf"
    assert granite["suggested"] is True  # see comment on the Qwen2.5-VTE assertion above
    assert granite["recipe_options"]["ctx_size"] == 8192  # same VRAM/tok-s fix as Qwen2.5-1.5B-Instruct-VTE

    qwen35 = data["Qwen3.5-2B-VTE"]
    assert qwen35["recipe"] == "vte"
    assert qwen35["checkpoint"] == "unsloth/Qwen3.5-2B-GGUF:Qwen3.5-2B-Q6_K.gguf"
    assert qwen35["suggested"] is True
    assert qwen35["recipe_options"]["ctx_size"] == 8192


def test_server_models_json_no_existing_entry_modified():
    """Confirms the only change to the file was the addition -- no existing
    entry was deleted or altered (simple regression check: a few known
    names from other entries are still present)."""
    data = json.loads(SERVER_MODELS.read_text(encoding="utf-8"))
    for known_model in ["Qwen3-0.6B-GGUF", "Tiny-Test-Model-GGUF", "Qwen3-1.7B-GGUF"]:
        assert known_model in data, f"Existing entry '{known_model}' disappeared -- regression!"


def test_backend_versions_json_valid_and_has_vte_entry():
    data = json.loads(BACKEND_VERSIONS.read_text(encoding="utf-8"))
    # 0.2.0: a real GitHub release (kyuubyN/VTE, tag "0.2.0", no "v" prefix --
    # see the earlier 0.1.0 release, same convention) with /v1/models,
    # a generation lock, and the downloader. Validated live before publishing.
    assert data.get("vte") == {"rocm": "0.2.0"}
    # Regression check: vllm stays intact.
    assert data.get("vllm") == {"rocm": "vllm0.20.1-rocm7.12.0"}


def test_capabilities_py_has_vte_entry_matching_moonshine_shape():
    sys.path.insert(0, str(CAPABILITIES_PY.parent))
    import capabilities  # noqa: E402

    vte_entry = capabilities.CAPABILITIES["llm"]["vte"]
    assert vte_entry["backends"] == ["rocm"]
    assert vte_entry["test_models"] == {"llm": "Qwen2.5-1.5B-Instruct-VTE"}

    required_keys = set(capabilities.CAPABILITIES["llm"]["ryzenai"]["supports"].keys())
    assert set(vte_entry["supports"].keys()) == required_keys, (
        "vte's 'supports' dict needs EXACTLY the same keys as another existing "
        "LLM backend (ryzenai) -- a missing key silently breaks "
        "skip_if_unsupported (treats it as 'not supported')."
    )
    # What's actually implemented and tested for real so far.
    assert vte_entry["supports"]["chat_completions"] is True
    assert vte_entry["supports"]["chat_completions_streaming"] is True
    # What's NOT implemented yet -- don't claim support that doesn't exist.
    assert vte_entry["supports"]["completions_streaming"] is False
    assert vte_entry["supports"]["embeddings"] is False
    assert vte_entry["supports"]["reranking"] is False
    assert vte_entry["supports"]["tool_calls"] is False
    # False: vte-server's own 80%-free-VRAM preflight refuses to load
    # alongside another already-loaded GPU model, contradicting multi_model
    # support (flagged in the maintainer PR review).
    assert vte_entry["supports"]["multi_model"] is False


def test_ci_workflow_yaml_valid_and_has_vte_row():
    workflow = yaml.safe_load(CI_WORKFLOW.read_text(encoding="utf-8"))
    matrix_include = workflow["jobs"]["test-exe-inference"]["strategy"]["matrix"]["include"]
    vte_rows = [row for row in matrix_include if row.get("name") == "vte"]
    assert len(vte_rows) == 1, "There should be exactly one 'vte' row in the CI matrix"
    row = vte_rows[0]
    assert row["extra_args"] == "--wrapped-server vte"
    assert row["backends"] == "rocm"
    assert "Windows" in row["runner"]
    assert "rocm" in row["runner"]


def test_ci_workflow_existing_rows_not_modified():
    workflow = yaml.safe_load(CI_WORKFLOW.read_text(encoding="utf-8"))
    matrix_include = workflow["jobs"]["test-exe-inference"]["strategy"]["matrix"]["include"]
    names = [row.get("name") for row in matrix_include]
    for known in ["llamacpp", "ryzenai", "flm", "moonshine"]:
        assert known in names, f"Existing CI row '{known}' disappeared -- regression!"

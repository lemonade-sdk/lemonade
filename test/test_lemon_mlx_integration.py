import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_lemon_mlx_static_integration_contract():
    """Keep lemon-mlx wired through the PR-sensitive integration points."""
    versions = json.loads(_read("src/cpp/resources/backend_versions.json"))
    assert set(versions["lemon-mlx"]) == {"metal", "rocm", "cpu"}
    assert all(versions["lemon-mlx"][backend] for backend in ("metal", "rocm", "cpu"))

    defaults = json.loads(_read("src/cpp/resources/defaults.json"))
    assert defaults["lemon-mlx"]["backend"] == "auto"
    assert defaults["lemon-mlx"]["metal_bin"] == "builtin"
    assert defaults["lemon-mlx"]["rocm_bin"] == "builtin"
    assert defaults["lemon-mlx"]["cpu_bin"] == "builtin"

    runtime_config = _read("src/cpp/server/runtime_config.cpp")
    assert '"lemon-mlx"' in runtime_config

    backend_manager = _read("src/cpp/server/backend_manager.cpp")
    assert 'recipe == "lemon-mlx"' in backend_manager
    assert "install_therock" in backend_manager

    router = _read("src/cpp/server/router.cpp")
    assert "MlxServer" in router
    assert 'model_info.recipe == "lemon-mlx"' in router

    mlx_server = _read("src/cpp/server/backends/mlx_server.cpp")
    assert "ROCM_HOME" in mlx_server
    assert "ROCM_PATH" in mlx_server
    assert "mlx-engine-" in mlx_server
    assert "max_completion_tokens" in mlx_server
    assert "reasoning_content" in mlx_server
    assert "<think>" in mlx_server
    assert "normalize_reasoning_response" in mlx_server
    assert "ReasoningStreamNormalizer" in mlx_server
    assert "prefers_prefix_reasoning" in mlx_server
    assert "is_small_qwen_model" in mlx_server
    assert 'device_type_ = (backend == "cpu") ? DEVICE_CPU : DEVICE_GPU' in mlx_server
    assert "drop_leading_stop_marker" in mlx_server
    assert "normalizer.stopped()" in mlx_server
    assert "locally_finished" in mlx_server
    assert "estimate_streamed_tokens" in mlx_server
    assert "estimate_prompt_tokens" in mlx_server
    assert "stream_options" in mlx_server
    assert "include_usage" in mlx_server
    assert 'endpoint == "/v1/completions"' in mlx_server
    assert "prefill_duration_ttft" in mlx_server
    assert "decoding_speed_tps" in mlx_server
    assert "<|im_start|>" in mlx_server
    assert "client_aborted" in mlx_server
    assert "restart_backend_after_cancel" in mlx_server
    assert "ensure_backend_ready" in mlx_server
    assert 'wait_for_ready("/health", 180)' in mlx_server
    assert "SmallQwenRepetitionStopper" in mlx_server
    assert "repetition_penalty" in mlx_server
    assert "emit_blocking_response_fallback" in mlx_server
    assert 'request["stream"] = true' in mlx_server

    mlx_header = _read("src/cpp/include/lemon/backends/mlx_server.h")
    assert "launch_executable_" in mlx_header
    assert "backend_restart_mutex_" in mlx_header


def test_lemon_mlx_models_are_modern_and_sized():
    models = json.loads(_read("src/cpp/resources/server_models.json"))
    expected = {
        "Qwen3.5-0.8B-MLX": "mlx-community/Qwen3.5-0.8B-8bit",
        "Qwen3.6-35B-A3B-MLX": "mlx-community/Qwen3.6-35B-A3B-4bit",
        "Qwen3.6-27B-MLX": "mlx-community/Qwen3.6-27B-4bit",
    }

    lemon_mlx_models = {
        name: model
        for name, model in models.items()
        if model.get("recipe") == "lemon-mlx"
    }

    assert set(lemon_mlx_models) == set(expected)

    for name, checkpoint in expected.items():
        model = lemon_mlx_models[name]
        assert model["recipe"] == "lemon-mlx"
        assert model["checkpoint"] == checkpoint
        assert model["suggested"] is True
        assert isinstance(model["size"], (int, float)) and model["size"] > 0


def test_lemon_mlx_capabilities_exercise_streaming_paths():
    capabilities = _read("test/utils/capabilities.py")
    lemon_mlx_block = capabilities.split('"lemon-mlx": {', 1)[1].split("        },", 1)[0]

    assert '"chat_completions_streaming": True' in lemon_mlx_block
    assert '"completions_streaming": True' in lemon_mlx_block
    assert '"tool_calls_streaming": True' in lemon_mlx_block
    assert '"llm": "Qwen3.5-0.8B-MLX"' in lemon_mlx_block


def test_readme_documents_lemon_mlx_without_overstating_support():
    readme = _read("README.md")
    assert "**MLX**" in readme
    assert "lemon-mlx" in readme
    assert "AMD Strix Halo iGPU" in readme
    assert '<td rowspan="3"><code>lemon-mlx</code> (experimental)</td>' in readme
    assert "MLX support" in readme.split("Recently Completed", 1)[1]

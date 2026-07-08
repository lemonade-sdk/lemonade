import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def _read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_lemon_mlx_static_integration_contract():
    """Keep lemon-mlx wired through the post-#2320 descriptor/registry contract."""
    cmake = _read("CMakeLists.txt")
    assert '"lemon-mlx|mlx"' in cmake

    descriptor = _read("src/cpp/include/lemon/backends/mlx/mlx.h")
    assert 'inline const BackendDescriptor descriptor' in descriptor
    assert '/*recipe*/          "lemon-mlx"' in descriptor
    assert '/*selectable_backend*/ true' in descriptor
    assert '{"lemon-mlx_backend", "--lemon-mlx"' in descriptor
    assert '{"lemon-mlx_args", "--lemon-mlx-args"' in descriptor
    assert '/*bin_variants*/    {"metal", "rocm", "cpu"}' in descriptor

    versions = json.loads(_read("src/cpp/resources/backend_versions.json"))
    assert set(versions["lemon-mlx"]) == {"metal", "rocm", "cpu"}
    assert all(versions["lemon-mlx"][backend] for backend in ("metal", "rocm", "cpu"))

    defaults = json.loads(_read("src/cpp/resources/defaults.json"))
    assert defaults["lemon-mlx"]["backend"] == "auto"
    assert defaults["lemon-mlx"]["args"] == ""
    assert defaults["lemon-mlx"]["metal_bin"] == "builtin"
    assert defaults["lemon-mlx"]["rocm_bin"] == "builtin"
    assert defaults["lemon-mlx"]["cpu_bin"] == "builtin"

    mlx_header = _read("src/cpp/include/lemon/backends/mlx/mlx_server.h")
    assert 'inline static const BackendSpec SPEC' not in mlx_header
    assert 'namespace mlx' in mlx_header
    assert 'const BackendSpec* spec();' in mlx_header
    assert 'const BackendOps* ops();' in mlx_header
    assert 'DeviceType effective_device' in mlx_header
    assert 'launch_executable_' in mlx_header
    assert 'backend_restart_mutex_' in mlx_header

    mlx_server = _read("src/cpp/server/backends/mlx/mlx_server.cpp")
    assert '#include "lemon/backends/mlx/mlx_server.h"' in mlx_server
    assert '#include "lemon/backends/mlx/mlx.h"' in mlx_server
    assert 'backend_manager_->install_backend(mlx::spec()->recipe, backend)' in mlx_server
    assert 'BackendUtils::get_backend_binary_path(*mlx::spec(), backend)' in mlx_server
    assert 'std::unique_ptr<WrappedServer> create(const BackendContext& ctx)' in mlx_server
    assert 'return make_server<MlxServer>(ctx);' in mlx_server
    assert 'return make_spec<MlxServer>(descriptor);' in mlx_server
    assert 'return default_backend_ops();' in mlx_server
    assert 'ROCM_HOME' in mlx_server
    assert 'ROCM_PATH' in mlx_server
    assert 'mlx-engine-' in mlx_server
    assert 'max_completion_tokens' in mlx_server
    assert 'reasoning_content' in mlx_server
    assert '<think>' in mlx_server
    assert 'normalize_reasoning_response' in mlx_server
    assert 'ReasoningStreamNormalizer' in mlx_server
    assert 'prefers_prefix_reasoning' in mlx_server
    assert 'is_small_qwen_model' in mlx_server
    assert 'return (backend == "cpu") ? DEVICE_CPU : DEVICE_GPU;' in mlx_server
    assert 'drop_leading_stop_marker' in mlx_server
    assert 'normalizer.stopped()' in mlx_server
    assert 'locally_finished' in mlx_server
    assert 'estimate_streamed_tokens' in mlx_server
    assert 'estimate_prompt_tokens' in mlx_server
    assert 'stream_options' in mlx_server
    assert 'include_usage' in mlx_server
    assert 'endpoint == "/v1/completions"' in mlx_server
    assert 'prefill_duration_ttft' in mlx_server
    assert 'decoding_speed_tps' in mlx_server
    assert '<|im_start|>' in mlx_server
    assert 'client_aborted' in mlx_server
    assert 'restart_backend_after_cancel' in mlx_server
    assert 'ensure_backend_ready' in mlx_server
    assert 'wait_for_ready("/health", 180)' in mlx_server
    assert 'SmallQwenRepetitionStopper' in mlx_server
    assert 'repetition_penalty' in mlx_server
    assert 'emit_blocking_response_fallback' in mlx_server
    assert 'request["stream"] = true' in mlx_server


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


def test_lemon_mlx_capabilities_match_supported_runtime_paths():
    capabilities = _read("test/utils/capabilities.py")
    assert '"lemon-mlx": {' in capabilities
    assert '"backends": ["metal", "rocm", "cpu"]' in capabilities
    assert '"chat_completions_streaming": True' in capabilities
    assert '"completions_streaming": False' in capabilities
    assert '"tool_calls_streaming": True' in capabilities
    assert '"llm": "Qwen3.5-0.8B-MLX"' in capabilities

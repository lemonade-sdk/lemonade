// Proves HRX keeps its narrow install, custom-argument, and process-launch
// contract. These checks call the same helpers as HrxServer::load(), without
// starting the backend or duplicating descriptor-derived data tables.

#include "lemon/backends/hrx/hrx.h"
#include "lemon/backends/hrx/hrx_server.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace hrx = lemon::backends::hrx;
using lemon::backends::HrxServer;

constexpr const char* kReleaseVersion = "hrx-b99";

int failures = 0;

void check(const std::string& what, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) {
        ++failures;
    }
}

bool throws_exception(const std::function<void()>& operation) {
    try {
        operation();
    } catch (const std::exception&) {
        return true;
    }
    return false;
}

void check_launch_contract() {
    const std::vector<std::string> expected_argv = {
        "-m", "/models/qualified.gguf",
        "--ctx-size", "32768",
        "--device", "HRX0",
        "--port", "14123",
        "--jinja",
        "--metrics",
        "--threads", "7",
        "--load-mode", "none",
        "--parallel", "1",
    };
    const auto argv = hrx::build_server_argv(
        "/models/qualified.gguf", 32768, 14123, "--threads 7 --load-mode none");
    check("HRX builds the complete managed argv with a benign custom tail",
          argv == expected_argv);

    const std::vector<std::string> expected_default_argv = {
        "-m", "/models/qualified.gguf",
        "--ctx-size", "32768",
        "--device", "HRX0",
        "--port", "14123",
        "--jinja",
        "--metrics",
        "--parallel", "1",
    };
    check("HRX pins --parallel 1 when no custom args are given",
          hrx::build_server_argv("/models/qualified.gguf", 32768, 14123, "") ==
              expected_default_argv);

    const std::vector<std::string> expected_override_argv = {
        "-m", "/models/qualified.gguf",
        "--ctx-size", "32768",
        "--device", "HRX0",
        "--port", "14123",
        "--jinja",
        "--metrics",
        "-np", "2",
    };
    check("HRX lets a custom -np override the --parallel default",
          hrx::build_server_argv("/models/qualified.gguf", 32768, 14123,
                                 "-np 2") == expected_override_argv);

    const std::vector<std::pair<std::string, std::string>> expected_environment = {
        {"GGML_DISABLE_VULKAN", "1"},
    };
    check("HRX builds the exact process environment",
          hrx::build_server_environment() == expected_environment);
}

void check_context_limit_contract() {
    lemon::ModelInfo qwen;
    qwen.model_name = "user.Qwen3.8-27B-HRX";
    qwen.checkpoints["main"] =
        "bartowski/Qwen3.8-27B-GGUF:Qwen3.8-27B-Q4_K_M.gguf";
    qwen.max_context_window = 262144;
    const lemon::backends::BackendOpsContext context;

    hrx::ops()->populate_metadata(qwen, context);
    check("HRX advertises the reported Qwen3.8-27B limit",
          qwen.max_context_window == 32768);

    lemon::ModelInfo qwen_without_metadata;
    qwen_without_metadata.model_name = "user.Qwen3.8-27B-HRX";
    qwen_without_metadata.checkpoints["main"] = qwen.checkpoint();
    hrx::ops()->populate_metadata(qwen_without_metadata, context);
    check("HRX supplies a known cap when GGUF metadata is unavailable",
          qwen_without_metadata.max_context_window == 32768);

    lemon::ModelInfo qwen_checkpoint_only;
    qwen_checkpoint_only.model_name = "user.HRX-model";
    qwen_checkpoint_only.checkpoints["main"] = qwen.checkpoint();
    qwen_checkpoint_only.max_context_window = 262144;
    hrx::ops()->populate_metadata(qwen_checkpoint_only, context);
    check("HRX identifies Qwen3.8-27B from its checkpoint",
          qwen_checkpoint_only.max_context_window == 32768);

    lemon::ModelInfo qwen_lower_context = qwen;
    qwen_lower_context.max_context_window = 16384;
    hrx::ops()->populate_metadata(qwen_lower_context, context);
    check("HRX preserves a lower detected context window",
          qwen_lower_context.max_context_window == 16384);

    lemon::RecipeOptions oversized("llamacpp-hrx", {{"ctx_size", 65536}});
    hrx::ops()->resolve_runtime_options(qwen, oversized);
    check("HRX clamps an explicit Qwen3.8-27B context size",
          oversized.get_option("ctx_size") == 32768);
    const auto qwen_launch = hrx::build_server_argv(
        "/models/qwen.gguf", oversized.get_option("ctx_size").get<int>(), 14123, "");
    check("HRX launches Qwen3.8-27B with the clamped context size",
          qwen_launch[2] == "--ctx-size" && qwen_launch[3] == "32768");

    lemon::RecipeOptions unsigned_oversized(
        "llamacpp-hrx", {{"ctx_size", std::numeric_limits<std::uint64_t>::max()}});
    hrx::ops()->resolve_runtime_options(qwen, unsigned_oversized);
    check("HRX clamps an oversized unsigned Qwen3.8-27B context size",
          unsigned_oversized.get_option("ctx_size") == 32768);

    lemon::RecipeOptions at_limit("llamacpp-hrx", {{"ctx_size", 32768}});
    hrx::ops()->resolve_runtime_options(qwen, at_limit);
    check("HRX preserves the exact Qwen3.8-27B boundary",
          at_limit.get_option("ctx_size") == 32768);

    lemon::RecipeOptions below_detected_limit("llamacpp-hrx", {{"ctx_size", 65536}});
    hrx::ops()->resolve_runtime_options(qwen_lower_context, below_detected_limit);
    check("HRX does not raise a lower detected Qwen3.8-27B limit",
          below_detected_limit.get_option("ctx_size") == 16384);

    lemon::RecipeOptions auto_sized("llamacpp-hrx", {{"ctx_size", -1}});
    hrx::ops()->resolve_runtime_options(qwen, auto_sized);
    check("HRX preserves automatic sizing for Qwen3.8-27B",
          auto_sized.get_option("ctx_size") == -1);

    lemon::ModelInfo other;
    other.model_name = "Qwen3-4B-Instruct-2507-HRX";
    other.checkpoints["main"] = "unsloth/Qwen3-4B-Instruct-2507-GGUF";
    other.max_context_window = 131072;
    hrx::ops()->populate_metadata(other, context);
    check("HRX leaves other models' context metadata unchanged",
          other.max_context_window == 131072);

    lemon::RecipeOptions other_options("llamacpp-hrx", {{"ctx_size", 65536}});
    hrx::ops()->resolve_runtime_options(other, other_options);
    check("HRX leaves other models' explicit context unchanged",
          other_options.get_option("ctx_size") == 65536);
}

void check_admission_contract() {
    const std::set<std::string> expected_reserved_flags = {
        "-c", "-dev", "-dr", "-hf", "-hff", "-hfr", "-m", "-mu",
        "--ctx-size", "--device", "--docker-repo", "--hf-file",
        "--hf-repo", "--jinja", "--metrics", "--model", "--model-url",
        "--no-jinja", "--port",
    };
    check("HRX reserves exactly its managed flags and aliases",
          hrx::reserved_custom_arg_flags() == expected_reserved_flags);

    const bool spaced_flag_rejected = throws_exception([] {
        (void)hrx::build_server_argv(
            "/models/qualified.gguf", 4096, 14123, "-m replacement.gguf");
    });
    check("HRX rejects a space-form managed argument", spaced_flag_rejected);

    const bool equals_flag_rejected = throws_exception([] {
        (void)hrx::build_server_argv(
            "/models/qualified.gguf", 4096, 14123, "--port=1");
    });
    check("HRX rejects an equals-form managed argument", equals_flag_rejected);
}

void check_installer_contract() {
#if defined(__linux__) && (defined(__x86_64__) || defined(__amd64__))
    const auto params = HrxServer::get_install_params("hrx", kReleaseVersion);
    check("HRX installer selects the exact release repository",
          params.repo == "ROCm/ggml-staging-automation");
    check("HRX installer maps the version to the exact release asset",
          params.filename ==
              "llama-hrx-b99-bin-manylinux-hrx-x64.tar.gz");
#else
    const bool unsupported_host_rejected = throws_exception([] {
        (void)HrxServer::get_install_params("hrx", kReleaseVersion);
    });
    check("HRX installer rejects a non-Linux-x86-64 host",
          unsupported_host_rejected);
#endif

    const bool wrong_backend_rejected = throws_exception([] {
        (void)HrxServer::get_install_params("rocm", kReleaseVersion);
    });
    check("HRX installer rejects another backend", wrong_backend_rejected);
}

}  // namespace

int main() {
    try {
        check_launch_contract();
        check_context_limit_contract();
        check_admission_contract();
        check_installer_contract();
    } catch (const std::exception& error) {
        check(std::string("unexpected exception: ") + error.what(), false);
    }

    if (failures == 0) {
        std::printf("\nAll HRX contract checks passed.\n");
        return 0;
    }
    std::printf("\n%d HRX contract check(s) failed.\n", failures);
    return 1;
}

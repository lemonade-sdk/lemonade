// Proves HRX keeps its narrow install, custom-argument, and process-launch
// contract. These checks resolve runtime options and call the launch helpers without
// starting the backend or duplicating descriptor-derived data tables.

#include "lemon/backends/backend_ops.h"
#include "lemon/backends/hrx/hrx.h"
#include "lemon/backends/hrx/hrx_server.h"
#include "lemon/model_manager.h"
#include "lemon/recipe_options.h"
#include "lemon/utils/custom_args.h"

#include <cstdio>
#include <exception>
#include <functional>
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

void check_mtp_contract() {
    lemon::ModelInfo info;
    info.labels = {"chat", "mtp"};
    lemon::RecipeOptions options("llamacpp-hrx", lemon::json::object());
    hrx::ops()->resolve_runtime_options(info, options);
    const auto args = hrx::build_server_argv(
        "/models/qualified.gguf", 4096, 14123, options.get_option("hrx_args"));
    check("MTP capability automatically reaches the HRX launch command",
          lemon::utils::custom_args_has_flag(args, "--spec-type"));
    check("MTP selects draft-mtp without overriding backend draft defaults",
          options.get_option("hrx_args") == "--spec-type draft-mtp");
    hrx::ops()->resolve_runtime_options(info, options);
    check("Repeated resolution does not duplicate MTP arguments",
          options.get_option("hrx_args") == "--spec-type draft-mtp");

    for (const std::string custom : {"--spec-type none", "--spec-type=none",
                                    "--spec-type draft-mtp --spec-draft-n-max 4"}) {
        lemon::RecipeOptions overridden("llamacpp-hrx", {{"hrx_args", custom}});
        hrx::ops()->resolve_runtime_options(info, overridden);
        check("Explicit speculative arguments win: " + custom,
              overridden.get_option("hrx_args") == custom);
    }

    lemon::RecipeOptions disabled("llamacpp-hrx", {{"merge_args", false},
                                                  {"hrx_args", "--threads 7"}});
    hrx::ops()->resolve_runtime_options(info, disabled);
    check("merge_args=false disables automatic MTP defaults",
          disabled.get_option("hrx_args") == "--threads 7");

    info.labels = {"chat"};
    lemon::RecipeOptions ordinary("llamacpp-hrx", {{"hrx_args", "--threads 7"}});
    hrx::ops()->resolve_runtime_options(info, ordinary);
    check("Non-MTP models retain their original arguments",
          ordinary.get_option("hrx_args") == "--threads 7");
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
        check_admission_contract();
        check_mtp_contract();
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

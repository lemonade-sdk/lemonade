// Proves HRX keeps its narrow install, custom-argument, and process-launch
// contract. These checks call the same helpers as HrxServer::load(), without
// starting the backend or duplicating descriptor-derived data tables.

#include "lemon/backends/hrx/hrx.h"
#include "lemon/backends/hrx/hrx_server.h"

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

constexpr const char* kReleaseVersion = "hrx-b69";

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
        "--no-mmap",
        "--parallel", "1",
    };
    const auto argv = hrx::build_server_argv(
        "/models/qualified.gguf", 32768, 14123, "--threads 7 --no-mmap");
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

void check_installer_contract() {
#if defined(__linux__) && (defined(__x86_64__) || defined(__amd64__))
    const auto params = HrxServer::get_install_params("hrx", kReleaseVersion);
    check("HRX installer selects the exact release repository",
          params.repo == "ROCm/ggml-staging-automation");
    check("HRX installer maps the version to the exact release asset",
          params.filename ==
              "llama-hrx-b69-bin-manylinux-hrx-x64.tar.gz");
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

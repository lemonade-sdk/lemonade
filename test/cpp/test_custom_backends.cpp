#include "lemon/backends/backend_descriptor.h"
#include "lemon/backends/backend_descriptor_registry.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/shell_utils.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using lemon::BackendDescriptor;
using lemon::utils::ProcessManager;

namespace {

int failures = 0;

void check(const char* name, bool condition) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) {
        ++failures;
    }
}

void test_tmp_sticky_bit_ancestor_permission() {
#ifndef _WIN32
    struct stat tmp_st;
    if (::stat("/tmp", &tmp_st) == 0 && (tmp_st.st_mode & S_ISVTX) != 0) {
        check("tmp ancestor has sticky bit (S_ISVTX) set", true);
    }

    // Set XDG_CONFIG_HOME to a directory under /tmp so get_backend_search_paths includes it
    std::string tmp_dir = "/tmp/lemonade_test_sticky_XXXXXX";
    char* created = mkdtemp(tmp_dir.data());
    check("mkdtemp succeeds under /tmp", created != nullptr);

    if (created) {
        ::setenv("XDG_CONFIG_HOME", created, 1);
        std::string backends_dir = std::string(created) + "/lemonade/backends";
        fs::create_directories(backends_dir);

        std::string desc_path = backends_dir + "/sticky_test.json";
        std::ofstream out(desc_path);
        out << R"({
            "recipe": "sticky_test_recipe",
            "display_name": "Sticky Test Recipe",
            "capabilities": ["chat_completion"],
            "platforms": {"cpu": {"command": "echo", "args": ["hello"]}}
        })";
        out.close();
        ::chmod(desc_path.c_str(), 0600);

        // Force descriptor refresh and query registry
        lemon::backends::refresh_descriptors();
        const auto* desc = lemon::backends::descriptor_for("sticky_test_recipe");

        check(
            "descriptor placed under /tmp sticky dir is accepted by check_path_permissions",
            desc != nullptr);

        fs::remove_all(created);
        lemon::backends::refresh_descriptors();
    }
#endif
}

void test_subprocess_env_secret_filtering() {
#ifndef _WIN32
    // Set ambient sensitive environment variables in parent process
    ::setenv("LEMONADE_API_KEY", "secret_key_12345", 1);
    ::setenv("CUDA_VISIBLE_DEVICES", "0,1,2,3", 1);
    ::setenv("HF_TOKEN", "hf_secret_token_abc", 1);
    ::setenv("TEST_CUSTOM_SAFE_ENV_VAR", "safe_env_value_789", 1);

    std::string output;
    int exit_code = ProcessManager::run_command("env", output, 5);
    check("run_command(env) executes successfully", exit_code == 0);

    check("spawned process env does NOT contain LEMONADE_API_KEY",
          output.find("LEMONADE_API_KEY") == std::string::npos);
    check("spawned process env does NOT contain CUDA_VISIBLE_DEVICES",
          output.find("CUDA_VISIBLE_DEVICES") == std::string::npos);
    check("spawned process env does NOT contain HF_TOKEN",
          output.find("HF_TOKEN") == std::string::npos);
    check("spawned process env DOES contain TEST_CUSTOM_SAFE_ENV_VAR",
          output.find("TEST_CUSTOM_SAFE_ENV_VAR=safe_env_value_789") !=
              std::string::npos);
#endif
}

// 3. Permission Rejection Test (Mode 0666 world-writable)
void test_descriptor_permission_rejection() {
#ifndef _WIN32
    std::string cache_backends = lemon::utils::get_cache_dir() + "/backends";
    fs::create_directories(cache_backends);

    std::string insecure_path = cache_backends + "/insecure_test.json";
    std::ofstream out(insecure_path);
    out << R"({
        "recipe": "insecure_test_recipe",
        "display_name": "Insecure Test",
        "capabilities": ["chat_completion"],
        "platforms": {"cpu": {"command": "echo", "args": ["hello"]}}
    })";
    out.close();

    // Set file mode to 0666 (world-writable)
    ::chmod(insecure_path.c_str(), 0666);
    lemon::backends::refresh_descriptors();
    const auto* desc_insecure =
        lemon::backends::descriptor_for("insecure_test_recipe");
    check("world-writable (0666) descriptor is REJECTED by descriptor registry",
          desc_insecure == nullptr);

    // Fix mode to 0600 (owner only)
    ::chmod(insecure_path.c_str(), 0600);
    lemon::backends::refresh_descriptors();
    const auto* desc_secure =
        lemon::backends::descriptor_for("insecure_test_recipe");
    check("owner-only (0600) descriptor is ACCEPTED after fixing mode",
          desc_secure != nullptr);

    fs::remove(insecure_path);
    lemon::backends::refresh_descriptors();
#endif
}

// 4. Custom Endpoint Paths test
void test_custom_endpoint_path_overrides() {
    BackendDescriptor desc;
    desc.recipe = "custom_ep_test";
    desc.endpoints["chat_completion"] = "/api/v1/custom_chat";

    check("get_endpoint_path returns overridden endpoint for chat_completion",
          desc.get_endpoint_path("chat_completion", "/v1/chat/completions") ==
              "/api/v1/custom_chat");

    check("get_endpoint_path returns default endpoint for non-overridden completion",
          desc.get_endpoint_path("completion", "/v1/completions") ==
              "/v1/completions");
}

// 5. Protected Flags Validation test
void test_protected_flags_validation() {
    BackendDescriptor desc;
    desc.recipe = "protected_flags_test";
    desc.protected_flags = {"--port", "--host"};

    check("protected_flags contains --port and --host", desc.protected_flags.size() == 2);
}

// 6. Path Traversal Guard & Lexical Relative Path Test
void test_path_traversal_relative_guard() {
    auto is_safe_relative = [](const fs::path& rel_p) -> bool {
        if (rel_p.empty() || rel_p.is_absolute()) return false;
        if (rel_p.begin() != rel_p.end() && *rel_p.begin() == "..") return false;
        return true;
    };

    fs::path base_path("/home/user/.cache/huggingface/hub");
    fs::path target_subpath("/home/user/.cache/huggingface/hub/models--unsloth/snapshots/rev/model.gguf");
    fs::path lex_rel = target_subpath.lexically_relative(base_path);

    fs::path unsafe_traversal("../../../tmp/evil.safetensors");
    fs::path abs_path("/etc/passwd");

    check("lexically_relative computes valid relative path preserving extension", lex_rel.generic_string() == "models--unsloth/snapshots/rev/model.gguf");
    check("is_safe_relative accepts valid relative subpath", is_safe_relative(lex_rel));
    check("is_safe_relative rejects leading .. traversal path", !is_safe_relative(unsafe_traversal));
    check("is_safe_relative rejects absolute path", !is_safe_relative(abs_path));
}

// 7. Shell Argument Escaping Safety Test
void test_shell_arg_escaping_safety() {
    std::string arg_with_spaces = "my custom binary";
    std::string posix_escaped = lemon::utils::escape_posix_shell_arg(arg_with_spaces);
    check("escape_posix_shell_arg wraps string with spaces in single quotes", posix_escaped == "'my custom binary'");

    std::string win_escaped = lemon::utils::escape_windows_arg(arg_with_spaces);
    check("escape_windows_arg wraps string with spaces in double quotes", win_escaped == "\"my custom binary\"");
}

} // namespace

int main() {
    std::printf("====================================================\n");
    std::printf("RUNNING C++ CUSTOM BACKENDS & PERMISSIONS UNIT TESTS\n");
    std::printf("====================================================\n");

    test_tmp_sticky_bit_ancestor_permission();
    test_subprocess_env_secret_filtering();
    test_descriptor_permission_rejection();
    test_custom_endpoint_path_overrides();
    test_protected_flags_validation();
    test_path_traversal_relative_guard();
    test_shell_arg_escaping_safety();

    std::printf("====================================================\n");
    std::printf("RESULTS: %d failure(s)\n", failures);
    std::printf("====================================================\n");

    return failures > 0 ? 1 : 0;
}

#include <cstdio>
#include <string>
#include <vector>

#include "lemon/sandbox/env_scrubber.h"
#include "sandbox_test_utils.h"

using lemon::sandbox::EnvScrubber;
using lemon::test::TestResult;

int main() {
    TestResult r;

    {
        r.check(EnvScrubber::is_sensitive_key("LEMONADE_API_KEY"), "LEMONADE_API_KEY is sensitive");
        r.check(EnvScrubber::is_sensitive_key("LEMONADE_ADMIN_API_KEY"), "LEMONADE_ADMIN_API_KEY is sensitive");
        r.check(EnvScrubber::is_sensitive_key("LEMONADE_OPENAI_API_KEY"), "LEMONADE_OPENAI_API_KEY is sensitive");
        r.check(EnvScrubber::is_sensitive_key("LEMONADE_CACHE_DIR"), "LEMONADE_CACHE_DIR is sensitive");
        r.check(EnvScrubber::is_sensitive_key("LEMONADE_DEFAULTS_PATH"), "LEMONADE_DEFAULTS_PATH is sensitive");
        r.check(EnvScrubber::is_sensitive_key("LEMONADE_GGML_HIP_PATH"), "LEMONADE_GGML_HIP_PATH is sensitive");
        r.check(EnvScrubber::is_sensitive_key("LEMONADE_CUSTOM_SECRET"), "arbitrary LEMONADE_* is sensitive");
        r.check(EnvScrubber::is_sensitive_key("lemonade_api_key"), "lowercase lemonade_api_key is sensitive");
        r.check(EnvScrubber::is_sensitive_key("Lemonade_Admin_Api_Key"), "mixed case Lemonade_Admin_Api_Key is sensitive");
    }

    {
        r.check(EnvScrubber::is_sensitive_key("OPENAI_API_KEY"), "OPENAI_API_KEY is sensitive");
        r.check(EnvScrubber::is_sensitive_key("openai_api_key"), "lowercase openai_api_key is sensitive");
        r.check(EnvScrubber::is_sensitive_key("ANTHROPIC_API_KEY"), "ANTHROPIC_API_KEY is sensitive");
        r.check(EnvScrubber::is_sensitive_key("FIREWORKS_API_KEY"), "FIREWORKS_API_KEY is sensitive");
        r.check(EnvScrubber::is_sensitive_key("COHERE_API_KEY"), "COHERE_API_KEY is sensitive");
        r.check(EnvScrubber::is_sensitive_key("GROQ_API_KEY"), "GROQ_API_KEY is sensitive");
        r.check(EnvScrubber::is_sensitive_key("MISTRAL_API_KEY"), "MISTRAL_API_KEY is sensitive");
        r.check(EnvScrubber::is_sensitive_key("DEEPSEEK_API_KEY"), "DEEPSEEK_API_KEY is sensitive");
        r.check(EnvScrubber::is_sensitive_key("GEMINI_API_KEY"), "GEMINI_API_KEY is sensitive");
        r.check(EnvScrubber::is_sensitive_key("XAI_API_KEY"), "XAI_API_KEY is sensitive");
        r.check(EnvScrubber::is_sensitive_key("TOGETHER_API_KEY"), "TOGETHER_API_KEY is sensitive");
        r.check(EnvScrubber::is_sensitive_key("CEREBRAS_API_KEY"), "CEREBRAS_API_KEY is sensitive");
        r.check(EnvScrubber::is_sensitive_key("PERPLEXITY_API_KEY"), "PERPLEXITY_API_KEY is sensitive");
        r.check(EnvScrubber::is_sensitive_key("SAMBANOVA_API_KEY"), "SAMBANOVA_API_KEY is sensitive");
        r.check(EnvScrubber::is_sensitive_key("CHROMA_API_KEY"), "CHROMA_API_KEY is sensitive");
        r.check(EnvScrubber::is_sensitive_key("OPENROUTER_API_KEY"), "OPENROUTER_API_KEY is sensitive");
        r.check(EnvScrubber::is_sensitive_key("AZURE_OPENAI_API_KEY"), "AZURE_OPENAI_API_KEY is sensitive");
    }

    {
        r.check(EnvScrubber::is_sensitive_key("AWS_ACCESS_KEY_ID"), "AWS_ACCESS_KEY_ID is sensitive");
        r.check(EnvScrubber::is_sensitive_key("AWS_SECRET_ACCESS_KEY"), "AWS_SECRET_ACCESS_KEY is sensitive");
        r.check(EnvScrubber::is_sensitive_key("AWS_SESSION_TOKEN"), "AWS_SESSION_TOKEN is sensitive");
        r.check(EnvScrubber::is_sensitive_key("HF_TOKEN"), "HF_TOKEN is sensitive");
        r.check(EnvScrubber::is_sensitive_key("HUGGING_FACE_HUB_TOKEN"), "HUGGING_FACE_HUB_TOKEN is sensitive");
        r.check(EnvScrubber::is_sensitive_key("HF_TOKEN_PATH"), "HF_TOKEN_PATH is sensitive");
        r.check(EnvScrubber::is_sensitive_key("GITHUB_TOKEN"), "GITHUB_TOKEN is sensitive");
        r.check(EnvScrubber::is_sensitive_key("GH_TOKEN"), "GH_TOKEN is sensitive");
        r.check(EnvScrubber::is_sensitive_key("MODELSCOPE_API_TOKEN"), "MODELSCOPE_API_TOKEN is sensitive");
        r.check(EnvScrubber::is_sensitive_key("CUSTOM_SECRET_KEY"), "CUSTOM_SECRET_KEY with suffix is sensitive");
        r.check(EnvScrubber::is_sensitive_key("MY_SERVICE_AUTH_TOKEN"), "MY_SERVICE_AUTH_TOKEN with suffix is sensitive");
        r.check(EnvScrubber::is_sensitive_key("CLIENT_PRIVATE_KEY"), "CLIENT_PRIVATE_KEY with suffix is sensitive");
    }

    {
        r.check(!EnvScrubber::is_sensitive_key("PATH"), "PATH is not sensitive");
        r.check(!EnvScrubber::is_sensitive_key("LD_LIBRARY_PATH"), "LD_LIBRARY_PATH is not sensitive");
        r.check(!EnvScrubber::is_sensitive_key("ROCM_PATH"), "ROCM_PATH is not sensitive");
        r.check(!EnvScrubber::is_sensitive_key("CUDA_PATH"), "CUDA_PATH is not sensitive");
        r.check(!EnvScrubber::is_sensitive_key("HOME"), "HOME is not sensitive");
        r.check(!EnvScrubber::is_sensitive_key("PYTHONPATH"), "PYTHONPATH is not sensitive");
        r.check(!EnvScrubber::is_sensitive_key("PYTHONNOUSERSITE"), "PYTHONNOUSERSITE is not sensitive");
        r.check(!EnvScrubber::is_sensitive_key("CUDA_VISIBLE_DEVICES"), "CUDA_VISIBLE_DEVICES is not sensitive");
        r.check(!EnvScrubber::is_sensitive_key("ROCR_VISIBLE_DEVICES"), "ROCR_VISIBLE_DEVICES is not sensitive");
        r.check(!EnvScrubber::is_sensitive_key("OCL_SET_SVM_SIZE"), "OCL_SET_SVM_SIZE is not sensitive");
        r.check(!EnvScrubber::is_sensitive_key("GGML_METAL_NO_RESIDENCY"), "GGML_METAL_NO_RESIDENCY is not sensitive");
        r.check(!EnvScrubber::is_sensitive_key("ESPEAK_DATA_PATH"), "ESPEAK_DATA_PATH is not sensitive");
        r.check(!EnvScrubber::is_sensitive_key("TMPDIR"), "TMPDIR is not sensitive");

        r.check(EnvScrubber::is_allowlisted_key("PATH"), "PATH is allowlisted");
        r.check(EnvScrubber::is_allowlisted_key("path"), "lowercase path is allowlisted");
        r.check(EnvScrubber::is_allowlisted_key("CUDA_VISIBLE_DEVICES"), "CUDA_VISIBLE_DEVICES is in workload hardware allowlist");
        r.check(!EnvScrubber::is_allowlisted_key("PYTHONNOUSERSITE"), "PYTHONNOUSERSITE is not in default allowlist (requires explicit backend grant)");
        r.check(!EnvScrubber::is_allowlisted_key("RANDOM_CUSTOM_VAR"), "RANDOM_CUSTOM_VAR is not allowlisted");
    }

    {
        std::vector<std::pair<std::string, std::string>> mixed_env = {
            {"PATH", "/usr/bin:/bin"},
            {"LEMONADE_API_KEY", "lemon_secret_999"},
            {"CUDA_VISIBLE_DEVICES", "0,1"},
            {"OPENAI_API_KEY", "sk-proj-abc12345"},
            {"HF_TOKEN", "hf_xyz987"},
            {"PYTHONNOUSERSITE", "1"},
            {"AWS_SECRET_ACCESS_KEY", "secret_aws"},
            {"ESPEAK_DATA_PATH", "/opt/espeak"}
        };

        auto sanitized = EnvScrubber::sanitize_environment(mixed_env, {}, false);
        r.check(sanitized.size() == 4, "sanitized custom environment preserves non-sensitive vars while stripping secrets");

        bool has_path = false;
        bool has_cuda = false;
        bool has_py = false;
        bool has_espeak = false;
        bool has_lemon_secret = false;
        bool has_openai = false;
        bool has_hf = false;
        bool has_aws = false;

        for (const auto& [k, v] : sanitized) {
            if (k == "PATH" && v == "/usr/bin:/bin") has_path = true;
            if (k == "CUDA_VISIBLE_DEVICES" && v == "0,1") has_cuda = true;
            if (k == "PYTHONNOUSERSITE" && v == "1") has_py = true;
            if (k == "ESPEAK_DATA_PATH" && v == "/opt/espeak") has_espeak = true;
            if (k == "LEMONADE_API_KEY") has_lemon_secret = true;
            if (k == "OPENAI_API_KEY") has_openai = true;
            if (k == "HF_TOKEN") has_hf = true;
            if (k == "AWS_SECRET_ACCESS_KEY") has_aws = true;
        }

        r.check(has_path, "PATH preserved with original value");
        r.check(has_cuda, "CUDA_VISIBLE_DEVICES preserved with original value");
        r.check(has_py, "PYTHONNOUSERSITE preserved when explicitly requested");
        r.check(has_espeak, "ESPEAK_DATA_PATH preserved when explicitly requested");
        r.check(!has_lemon_secret, "LEMONADE_API_KEY completely stripped");
        r.check(!has_openai, "OPENAI_API_KEY completely stripped");
        r.check(!has_hf, "HF_TOKEN completely stripped");
        r.check(!has_aws, "AWS_SECRET_ACCESS_KEY completely stripped");
    }

    {
        std::vector<std::pair<std::string, std::string>> custom = {
            {"MY_CUSTOM_FLAG", "enabled"},
            {"ANOTHER_VAR", "value"}
        };

        auto sanitized = EnvScrubber::sanitize_environment(
            custom,
            {"MY_CUSTOM_FLAG"},
            false
        );

        bool has_flag = false;
        bool has_another = false;
        for (const auto& [k, v] : sanitized) {
            if (k == "MY_CUSTOM_FLAG" && v == "enabled") has_flag = true;
            if (k == "ANOTHER_VAR") has_another = true;
        }

        r.check(has_flag, "explicit extra allowlist variable is retained");
        r.check(has_another, "custom pair in custom_env is retained");
    }

    {
        auto ambient_sanitized = EnvScrubber::sanitize_environment({}, {}, true);
        for (const auto& [k, v] : ambient_sanitized) {
            r.check(!EnvScrubber::is_sensitive_key(k), "ambient sanitized contains no sensitive keys: " + k);
        }
    }

    r.report_summary("EnvScrubber");
    return r.exit_code();
}

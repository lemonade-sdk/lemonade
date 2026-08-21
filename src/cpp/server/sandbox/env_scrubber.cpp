#include "lemon/sandbox/env_scrubber.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <processenv.h>
#else
#include <unistd.h>
extern char** environ;
#endif

namespace lemon::sandbox {

namespace {

std::string to_upper_ascii(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return str;
}

bool starts_with(const std::string& str, const std::string& prefix) {
    return str.size() >= prefix.size() &&
           str.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& str, const std::string& suffix) {
    return str.size() >= suffix.size() &&
           str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

} // namespace

const std::vector<std::string>& EnvScrubber::sensitive_prefixes() {
    static const std::vector<std::string> prefixes = {
        "LEMONADE_",
        "AWS_",
        "AZURE_"
    };
    return prefixes;
}

const std::vector<std::string>& EnvScrubber::sensitive_suffixes() {
    static const std::vector<std::string> suffixes = {
        "_API_KEY",
        "_API_TOKEN",
        "_SECRET_KEY",
        "_SECRET_TOKEN",
        "_ACCESS_KEY",
        "_AUTH_TOKEN",
        "_BEARER_TOKEN",
        "_PRIVATE_KEY"
    };
    return suffixes;
}

const std::unordered_set<std::string>& EnvScrubber::sensitive_exact_keys() {
    static const std::unordered_set<std::string> keys = {
        "OPENAI_API_KEY",
        "ANTHROPIC_API_KEY",
        "FIREWORKS_API_KEY",
        "COHERE_API_KEY",
        "CO_API_KEY",
        "GROQ_API_KEY",
        "MISTRAL_API_KEY",
        "DEEPSEEK_API_KEY",
        "GEMINI_API_KEY",
        "GOOGLE_API_KEY",
        "PALM_API_KEY",
        "XAI_API_KEY",
        "TOGETHER_API_KEY",
        "TOGETHERAI_API_KEY",
        "CEREBRAS_API_KEY",
        "PERPLEXITY_API_KEY",
        "PPLX_API_KEY",
        "SAMBANOVA_API_KEY",
        "CHROMA_API_KEY",
        "CHROMA_SERVER_AUTH_CREDENTIALS",
        "OPENROUTER_API_KEY",
        "AZURE_OPENAI_API_KEY",
        "VOYAGE_API_KEY",
        "REPLICATE_API_TOKEN",
        "ANYSCALE_API_KEY",
        "AI21_API_KEY",
        "OCTOAI_API_KEY",
        "NOVITA_API_KEY",
        "RUNPOD_API_KEY",
        "FAL_KEY",
        "CLOUDFLARE_API_TOKEN",

        "HF_TOKEN",
        "HUGGING_FACE_HUB_TOKEN",
        "HF_API_TOKEN",
        "HUGGINGFACE_TOKEN",
        "HF_TOKEN_PATH",
        "MODELSCOPE_API_TOKEN",
        "KAGGLE_KEY",
        "KAGGLE_USERNAME",

        "GITHUB_TOKEN",
        "GH_TOKEN",
        "GITLAB_TOKEN",
        "BITBUCKET_TOKEN",
        "WANDB_API_KEY",
        "COMET_API_KEY",
        "LANGCHAIN_API_KEY",
        "LANGSMITH_API_KEY",

        "AWS_ACCESS_KEY_ID",
        "AWS_SECRET_ACCESS_KEY",
        "AWS_SESSION_TOKEN",
        "AWS_SECURITY_TOKEN",
        "AZURE_CLIENT_ID",
        "AZURE_CLIENT_SECRET",
        "AZURE_TENANT_ID",
        "GOOGLE_APPLICATION_CREDENTIALS",
        "SSH_AUTH_SOCK",
        "SSH_AGENT_PID",
        "GPG_AGENT_INFO"
    };
    return keys;
}

const std::unordered_set<std::string>& EnvScrubber::default_allowlist() {
    static const std::unordered_set<std::string> allowlist = {
        "PATH",
        "HOME",
        "USER",
        "LOGNAME",
        "USERNAME",
        "LANG",
        "LC_ALL",
        "LC_CTYPE",
        "LC_MESSAGES",
        "TERM",
        "SHELL",
        "TZ",

        "TMPDIR",
        "TEMP",
        "TMP",

        "HIP_VISIBLE_DEVICES",
        "ROCR_VISIBLE_DEVICES",
        "HSA_OVERRIDE_GFX_VERSION",
        "OCL_SET_SVM_SIZE",

        "CUDA_VISIBLE_DEVICES",
        "CUDA_DEVICE_ORDER",
        "NVIDIA_VISIBLE_DEVICES",

        "VK_ICD_FILENAMES",
        "VK_DRIVER_FILES",
        "VK_LAYER_PATH",

        "XILINX_XRT",
        "XLNX_VART_FIRMWARE",
        "FLM_CACHE_DIR",
        "FASTFLOWLM_CACHE_DIR",

        "SYSTEMROOT",
        "SYSTEMDRIVE",
        "WINDIR",
        "COMSPEC",
        "PATHEXT",
        "LOCALAPPDATA",
        "APPDATA",
        "PROGRAMDATA",
        "PROGRAMFILES",
        "PROGRAMFILES(X86)",
        "COMMONPROGRAMFILES",
        "USERPROFILE",
        "ALLUSERSPROFILE",
        "NUMBER_OF_PROCESSORS",
        "PROCESSOR_ARCHITECTURE",
        "PROCESSOR_IDENTIFIER",
        "PROCESSOR_LEVEL",
        "PROCESSOR_REVISION",
        "OS"
    };
    return allowlist;
}

bool EnvScrubber::is_sensitive_key(const std::string& key) {
    if (key.empty()) {
        return false;
    }
    const std::string upper = to_upper_ascii(key);

    for (const auto& prefix : sensitive_prefixes()) {
        if (starts_with(upper, prefix)) {
            return true;
        }
    }

    for (const auto& suffix : sensitive_suffixes()) {
        if (ends_with(upper, suffix)) {
            return true;
        }
    }

    return sensitive_exact_keys().count(upper) > 0;
}

bool EnvScrubber::is_allowlisted_key(const std::string& key) {
    if (key.empty()) {
        return false;
    }
    return default_allowlist().count(to_upper_ascii(key)) > 0;
}

std::vector<std::pair<std::string, std::string>> EnvScrubber::get_ambient_environment() {
    std::vector<std::pair<std::string, std::string>> ambient;

#ifdef _WIN32
    LPWCH environment = GetEnvironmentStringsW();
    if (environment) {
        for (const wchar_t* entry = environment; *entry != L'\0';
             entry += std::wcslen(entry) + 1) {
            int size_needed = WideCharToMultiByte(CP_UTF8, 0, entry, -1, nullptr, 0, nullptr, nullptr);
            if (size_needed > 0) {
                std::string narrow(size_needed - 1, '\0');
                WideCharToMultiByte(CP_UTF8, 0, entry, -1, &narrow[0], size_needed, nullptr, nullptr);
                size_t eq_pos = narrow.find('=');
                if (eq_pos != std::string::npos && eq_pos > 0) {
                    ambient.emplace_back(narrow.substr(0, eq_pos), narrow.substr(eq_pos + 1));
                }
            }
        }
        FreeEnvironmentStringsW(environment);
    }
#else
    for (char** e = environ; e && *e; ++e) {
        std::string entry(*e);
        size_t eq_pos = entry.find('=');
        if (eq_pos != std::string::npos && eq_pos > 0) {
            ambient.emplace_back(entry.substr(0, eq_pos), entry.substr(eq_pos + 1));
        }
    }
#endif

    return ambient;
}

std::vector<std::pair<std::string, std::string>> EnvScrubber::sanitize_environment(
    const std::vector<std::pair<std::string, std::string>>& custom_env,
    const std::vector<std::string>& extra_allowlist,
    bool include_ambient) {

    std::unordered_map<std::string, std::pair<std::string, std::string>> sanitized_map;

    std::unordered_set<std::string> extra_allowed_set;
    for (const auto& extra : extra_allowlist) {
        extra_allowed_set.insert(to_upper_ascii(extra));
    }

    if (include_ambient) {
        auto ambient = get_ambient_environment();
        for (const auto& [k, v] : ambient) {
            const std::string upper = to_upper_ascii(k);

            if (is_sensitive_key(upper) && extra_allowed_set.count(upper) == 0) {
                continue;
            }

            if (is_allowlisted_key(upper) || extra_allowed_set.count(upper) > 0) {
                sanitized_map[upper] = {k, v};
            }
        }
    }

    for (const auto& [k, v] : custom_env) {
        const std::string upper = to_upper_ascii(k);

        if (is_sensitive_key(upper) && extra_allowed_set.count(upper) == 0) {
            continue;
        }

        sanitized_map[upper] = {k, v};
    }

    std::vector<std::pair<std::string, std::string>> result;
    result.reserve(sanitized_map.size());
    for (auto& [_, pair] : sanitized_map) {
        result.push_back(std::move(pair));
    }

    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    return result;
}

} // namespace lemon::sandbox

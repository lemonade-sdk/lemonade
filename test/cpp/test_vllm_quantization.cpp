#include "lemon/backends/vllm/vllm_quantization.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using lemon::backends::detect_vllm_quant_method;

namespace {
int failures = 0;
int checks = 0;

void expect(bool condition, const std::string& label) {
    ++checks;
    std::cout << (condition ? "PASS: " : "FAIL: ") << label << '\n';
    if (!condition) ++failures;
}

void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1);
    else unsetenv(name);
#endif
}

struct ScopedEnv {
    const char* name;
    std::optional<std::string> previous;
    explicit ScopedEnv(const char* key) : name(key) {
        if (const char* value = std::getenv(name)) previous = value;
        set_env(name, nullptr);
    }
    ~ScopedEnv() { set_env(name, previous ? previous->c_str() : nullptr); }
};

struct Scratch {
    fs::path root = fs::temp_directory_path() / ("vllm-quant-test-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() { fs::create_directories(root); }
    ~Scratch() { std::error_code ec; fs::remove_all(root, ec); }
};

void write_config(const fs::path& snapshot, const std::string& body) {
    fs::create_directories(snapshot);
    std::ofstream(snapshot / "config.json") << body;
}

const std::string awq = R"({"quantization_config":{"quant_method":"awq"}})";
const std::string fp16 = R"({"torch_dtype":"float16","model_type":"qwen3"})";
} // namespace

int main() {
    ScopedEnv lemonade_offline("LEMONADE_OFFLINE");
    ScopedEnv hf_offline("HF_HUB_OFFLINE");
    ScopedEnv transformers_offline("TRANSFORMERS_OFFLINE");
    Scratch scratch;
    const auto repo = scratch.root / "models--Qwen--Test";
    const auto active = repo / "snapshots" / "active";
    const auto stale = repo / "snapshots" / "stale";
    int requests = 0;
    auto fetch = [&]() { ++requests; return awq; };

    write_config(active, fp16);
    write_config(stale, awq);
    expect(detect_vllm_quant_method(active, repo, false, fetch).empty() && requests == 0,
           "active FP16 config overrides stale AWQ and makes zero requests");
    expect(detect_vllm_quant_method({}, scratch.root / "cached-fp16", false, fetch) == "awq"
               && requests == 1, "missing cache can still detect remote AWQ");
    requests = 0;
    const auto cached = scratch.root / "cached-fp16";
    write_config(cached / "snapshots" / "cached", fp16);
    expect(detect_vllm_quant_method({}, cached, false, fetch).empty() && requests == 0,
           "cached FP16 config without quantization metadata makes zero requests");
    expect(detect_vllm_quant_method(active, repo, true, fetch).empty() && requests == 0,
           "cached FP16 loads while offline");

    write_config(active, awq);
    expect(detect_vllm_quant_method(active, repo, true, fetch) == "awq" && requests == 0,
           "offline mode preserves active AWQ detection");
    const auto awq_repo = scratch.root / "cached-awq";
    write_config(awq_repo / "snapshots" / "cached", awq);
    expect(detect_vllm_quant_method({}, awq_repo, true, fetch) == "awq" && requests == 0,
           "offline mode preserves cached AWQ detection");
    write_config(active, R"({"quantization_config":{"quant_method":"gptq"}})");
    expect(detect_vllm_quant_method(active, repo, false, fetch) == "gptq" && requests == 0,
           "active GPTQ config overrides cached AWQ");

    expect(detect_vllm_quant_method({}, {}, true, fetch).empty() && requests == 0,
           "offline mode skips network even with no cache");
    for (const char* name : {"LEMONADE_OFFLINE", "HF_HUB_OFFLINE", "TRANSFORMERS_OFFLINE"}) {
        for (const char* value : {"1", "true", "YES", "on"}) {
            set_env(name, value);
            expect(detect_vllm_quant_method({}, {}, false, fetch).empty() && requests == 0,
                   std::string(name) + "=" + value + " skips network");
        }
        set_env(name, "0");
        expect(detect_vllm_quant_method({}, {}, false, fetch) == "awq" && requests == 1,
               std::string(name) + "=0 allows remote detection");
        requests = 0;
        set_env(name, nullptr);
    }

    auto throwing_fetch = [&]() -> std::string {
        ++requests;
        throw std::runtime_error("CURL error: Could not resolve hostname");
    };
    expect(detect_vllm_quant_method({}, {}, false, throwing_fetch).empty() && requests == 1,
           "DNS/transport failure falls back to vLLM auto-selection");
    requests = 0;
    auto invalid_fetch = [&]() { ++requests; return "not JSON"; };
    expect(detect_vllm_quant_method({}, {}, false, invalid_fetch).empty() && requests == 1,
           "invalid remote config is nonfatal");
    auto fp16_fetch = [&]() { return fp16; };
    expect(detect_vllm_quant_method({}, {}, false, fp16_fetch).empty(),
           "remote FP16 config allows auto-selection");
    expect(detect_vllm_quant_method({}, {}, false, []() { return std::string(); }).empty(),
           "unsuccessful HTTP response allows auto-selection");
    expect(detect_vllm_quant_method({}, {}, false, {}).empty(),
           "unavailable remote lookup allows auto-selection");

    write_config(active, "{invalid JSON");
    requests = 0;
    expect(detect_vllm_quant_method(active, {}, false, fetch) == "awq" && requests == 1,
           "malformed local config permits remote fallback");
    requests = 0;
    expect(detect_vllm_quant_method(active, {}, true, fetch).empty() && requests == 0,
           "malformed local config never forces network in offline mode");
    expect(detect_vllm_quant_method(active, repo, false, fetch) == "awq" && requests == 0,
           "malformed active config falls back to readable cached config");

    for (const char* body : {"{}", "{\"quantization_config\":null}",
                            "{\"quantization_config\":{}}"}) {
        write_config(active, body);
        expect(detect_vllm_quant_method(active, repo, false, fetch).empty() && requests == 0,
               std::string("valid unquantized config skips network: ") + body);
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}

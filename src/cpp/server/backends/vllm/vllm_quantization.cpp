#include "lemon/backends/vllm/vllm_quantization.h"

#include <lemon/utils/aixlog.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <optional>
#include <sstream>

namespace fs = std::filesystem;

namespace lemon {
namespace backends {
namespace {

std::optional<std::string> parse_quant_method(const std::string& body) {
    const auto config = nlohmann::json::parse(body, nullptr, false);
    if (!config.is_object()) return std::nullopt;
    const auto quant = config.find("quantization_config");
    if (quant != config.end() && quant->is_object()) {
        const auto method = quant->find("quant_method");
        if (method != quant->end() && method->is_string()) {
            return method->get<std::string>();
        }
    }
    // A valid config without quantization metadata is normal for FP16 models.
    return std::string();
}

std::optional<std::string> read_quant_method(const fs::path& snapshot) {
    if (snapshot.empty()) return std::nullopt;
    std::ifstream file(snapshot / "config.json");
    if (!file) return std::nullopt;
    std::stringstream body;
    body << file.rdbuf();
    return parse_quant_method(body.str());
}

bool env_enabled(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr) return false;
    std::string normalized(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return normalized == "1" || normalized == "TRUE" || normalized == "YES" ||
           normalized == "ON";
}

} // namespace

std::string detect_vllm_quant_method(
    const fs::path& local_snapshot,
    const fs::path& repo_cache,
    bool offline,
    const std::function<std::string()>& fetch_config) {
    if (auto method = read_quant_method(local_snapshot)) return *method;

    if (!repo_cache.empty()) {
        std::error_code ec;
        fs::directory_iterator snapshots(repo_cache / "snapshots",
                                        fs::directory_options::skip_permission_denied, ec);
        for (fs::directory_iterator end; !ec && snapshots != end; snapshots.increment(ec)) {
            if (auto method = read_quant_method(snapshots->path())) return *method;
        }
    }

    if (offline || env_enabled("LEMONADE_OFFLINE") || env_enabled("HF_HUB_OFFLINE") ||
        env_enabled("TRANSFORMERS_OFFLINE") || !fetch_config) {
        return "";
    }

    try {
        return parse_quant_method(fetch_config()).value_or("");
    } catch (const std::exception& e) {
        LOG(DEBUG, "vLLM") << "Could not fetch config.json; skipping quant detection: "
                           << e.what() << std::endl;
        return "";
    }
}

} // namespace backends
} // namespace lemon

#include "lemon_cli/junie_profile.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace lemon_cli {
namespace {

// Junie stores its data under ~/.junie by default (%USERPROFILE%\.junie on
// Windows), or in $JUNIE_HOME if set. Custom model profiles live in the models/
// subdirectory, one JSON file per model. We always write the "lemonade" profile.
std::string resolve_junie_models_path() {
    const char* junie_home = std::getenv("JUNIE_HOME");
    if (junie_home && junie_home[0] != '\0') {
        return (fs::path(junie_home) / "models" / "lemonade.json").string();
    }

#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    if (!home || home[0] == '\0') {
        return "";
    }
    return (fs::path(home) / ".junie" / "models" / "lemonade.json").string();
}

// Atomically write JSON to path, creating parent directories. Mirrors the
// approach in agent_config_file.cpp / pi_profile.cpp.
bool write_json_atomic(const std::string& path,
                       const nlohmann::json& config,
                       std::string& error_out) {
    const fs::path output_path(path);
    std::error_code ec;
    const fs::path output_dir = output_path.parent_path();
    if (!output_dir.empty()) {
        fs::create_directories(output_dir, ec);
        if (ec) {
            error_out = "Cannot create directory " + output_dir.string() + ": " + ec.message();
            return false;
        }
    }

    const fs::path tmp_path = output_path.string() + ".tmp";
    {
        std::ofstream out(tmp_path);
        if (!out.is_open()) {
            error_out = "Cannot open " + tmp_path.string() + " for writing";
            return false;
        }

        out << config.dump(2) << "\n";
        out.flush();
        if (!out.good()) {
            out.close();
            std::error_code remove_ec;
            fs::remove(tmp_path, remove_ec);
            error_out = "Write failed for " + tmp_path.string();
            return false;
        }

        out.close();
        if (!out) {
            std::error_code remove_ec;
            fs::remove(tmp_path, remove_ec);
            error_out = "Write failed for " + tmp_path.string();
            return false;
        }
    }

    fs::rename(tmp_path, output_path, ec);
    if (ec) {
        // Cross-device or filesystem edge cases can make rename fail.
        ec.clear();
        fs::copy_file(tmp_path, output_path, fs::copy_options::overwrite_existing, ec);
        if (!ec) {
            std::error_code remove_tmp_ec;
            fs::remove(tmp_path, remove_tmp_ec);
        }
    }

    if (ec) {
        std::error_code remove_ec;
        fs::remove(tmp_path, remove_ec);
        error_out = "Cannot move temp config into place: " + ec.message();
        return false;
    }

    return true;
}

} // namespace

bool sync_junie_model_file(const std::string& base_url,
                           const std::string& api_key,
                           const std::string& model_id,
                           std::string& error_out) {
    const std::string models_path = resolve_junie_models_path();
    if (models_path.empty()) {
        error_out = "Could not resolve junie models path";
        return false;
    }

    // The "lemonade" custom model profile is fully owned by Lemonade, so it is
    // rewritten wholesale on every launch (like the opencode/pi "Lemonade"
    // provider block). The file name is what `--model custom:lemonade` resolves,
    // while the `id` field carries the actual Lemonade model name.
    nlohmann::json profile = nlohmann::json::object();
    profile["id"] = model_id;
    profile["baseUrl"] = base_url;
    profile["apiType"] = "OpenAICompletion";
    if (!api_key.empty()) {
        profile["apiKey"] = api_key;
    }

    return write_json_atomic(models_path, profile, error_out);
}

} // namespace lemon_cli

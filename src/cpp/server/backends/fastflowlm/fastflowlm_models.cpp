#include "lemon/backends/fastflowlm/fastflowlm_models.h"

#include <cstdlib>
#include <vector>
#include <nlohmann/json.hpp>
#include "lemon/model_manager.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace lemon {
namespace backends {
namespace fastflowlm {
namespace {

using lemon::utils::path_from_utf8;

bool safe_exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

// Candidate roots that FLM may use to store models. FLM resolves its model
// directory from the FLM_MODEL_PATH env var (set by the installer) and falls
// back to platform-default locations.
std::vector<fs::path> get_flm_models_dir_candidates() {
    std::vector<fs::path> roots;

    const char* flm_model_path = std::getenv("FLM_MODEL_PATH");
    if (flm_model_path && *flm_model_path) {
        roots.push_back(path_from_utf8(flm_model_path) / "models");
    }

#ifdef _WIN32
    const char* userprofile = std::getenv("USERPROFILE");
    if (userprofile && *userprofile) {
        fs::path home = path_from_utf8(userprofile);
        roots.push_back(home / ".flm" / "models");              // current installer default
        roots.push_back(home / "Documents" / "flm" / "models"); // legacy installer default
        roots.push_back(home / "flm" / "models");
    }
#else
    const char* xdg_config_home = std::getenv("XDG_CONFIG_HOME");
    if (xdg_config_home && *xdg_config_home) {
        roots.push_back(path_from_utf8(xdg_config_home) / "flm" / "models");
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
        fs::path home_path = path_from_utf8(home);
        roots.push_back(home_path / ".flm" / "models");
        roots.push_back(home_path / ".config" / "flm" / "models");
    }
#endif

    return roots;
}

} // namespace

fs::path find_flm_config_path_from_repo_dir(const std::string& repo_dir) {
    if (repo_dir.empty()) return fs::path();

    for (const auto& root : get_flm_models_dir_candidates()) {
        fs::path candidate = root / repo_dir / "config.json";
        if (safe_exists(candidate)) return candidate;
    }
    return fs::path();
}

std::string repo_dir_from_url(const std::string& url) {
    std::string clean = url;
    while (!clean.empty() && clean.back() == '/') clean.pop_back();
    size_t query_pos = clean.find_first_of("?#");
    if (query_pos != std::string::npos) clean = clean.substr(0, query_pos);

    for (const std::string marker : {"/tree/", "/resolve/"}) {
        size_t marker_pos = clean.find(marker);
        if (marker_pos != std::string::npos) {
            clean = clean.substr(0, marker_pos);
            break;
        }
    }

    size_t slash = clean.find_last_of('/');
    return slash == std::string::npos ? clean : clean.substr(slash + 1);
}

int64_t read_flm_max_context_window(const ModelInfo& info) {
    if (info.type != ModelType::LLM) return 0;

    std::string config_path = info.resolved_path("config");
    if (config_path.empty()) return 0;

    try {
        json config = lemon::utils::JsonUtils::load_from_file(config_path);
        if (config.contains("max_position_embeddings") && config["max_position_embeddings"].is_number_integer()) {
            int64_t value = config["max_position_embeddings"].get<int64_t>();
            return value > 0 ? value : 0;
        }
        if (config.contains("text_config") && config["text_config"].is_object()) {
            const auto& text_config = config["text_config"];
            if (text_config.contains("max_position_embeddings") && text_config["max_position_embeddings"].is_number_integer()) {
                int64_t value = text_config["max_position_embeddings"].get<int64_t>();
                return value > 0 ? value : 0;
            }
        }
    } catch (const std::exception& e) {
        LOG(DEBUG, "FastFlowLM") << "Could not read FLM config metadata for "
                                 << info.model_name << ": " << e.what() << std::endl;
    }
    return 0;
}

} // namespace fastflowlm
} // namespace backends
} // namespace lemon

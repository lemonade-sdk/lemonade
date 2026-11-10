#include "lemon/backends/hf_cache_util.h"
#include "lemon/backends/onnxruntime/onnxruntime_hooks.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/process_manager.h"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <stdexcept>

namespace lemon::backends::onnxruntime {

namespace {
namespace fs = std::filesystem;

bool is_complete_model_dir(const fs::path& dir) {
    std::error_code ec;
    return fs::exists(dir / "model.onnx", ec) && fs::exists(dir / "tokenizer.json", ec) &&
           fs::exists(dir / "config.json", ec);
}

std::vector<fs::path> find_complete_model_dirs(const fs::path& root) {
    std::vector<fs::path> dirs;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, hf_cache::dir_options(), ec);
    if (ec) return dirs;
    for (auto end = fs::recursive_directory_iterator(); it != end; it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec) && !ec && it->path().filename() == "model.onnx" &&
            is_complete_model_dir(it->path().parent_path())) {
            dirs.push_back(it->path().parent_path());
        }
    }
    std::sort(dirs.begin(), dirs.end());
    return dirs;
}

class ModelDirectoryOps : public BackendOps {
public:
    std::string resolve_checkpoint_path(const ModelInfo&, const CheckpointResolveContext& ctx) const override {
        fs::path cache = utils::path_from_utf8(ctx.model_cache_path);
        fs::path active = hf_cache::active_snapshot_path(cache);
        if (!active.empty()) {
            std::string found = find_imported_checkpoint(utils::path_to_utf8(active));
            return found.empty() ? utils::path_to_utf8(active) : found;
        }
        std::string found = find_imported_checkpoint(ctx.model_cache_path);
        return found.empty() ? ctx.model_cache_path : found;
    }

    std::string find_imported_checkpoint(const std::string& import_dir) const override {
        fs::path dir = utils::path_from_utf8(import_dir);
        if (!hf_cache::exists(dir)) {
            return "";
        }
        auto candidates = find_complete_model_dirs(dir);
        if (candidates.size() != 1) {
            if (candidates.size() > 1) {
                LOG(WARNING, "OnnxRuntimeServer")
                    << candidates.size() << " complete model directories under "
                    << import_dir << "; refusing to pick one" << std::endl;
            }
            return "";
        }
        return utils::path_to_utf8(candidates.front());
    }
};
}

const BackendOps* model_directory_ops() {
    static const ModelDirectoryOps ops;
    return &ops;
}

json servable_directory(const LaunchHookContext& context) {
    const std::string model_path = context.values.at("artifacts").at("main");
    auto candidates = find_complete_model_dirs(utils::path_from_utf8(model_path));
    if (candidates.empty()) {
        throw std::runtime_error(
            "No servable model directory under '" + model_path +
            "': need model.onnx + tokenizer.json + config.json "
            "(manifest.json is optional and overrides the output contract)");
    }
    if (candidates.size() > 1) {
        std::string listing;
        for (const auto& c : candidates) listing += "\n  " + utils::path_to_utf8(c);
        throw std::runtime_error(
            "Ambiguous model layout under '" + model_path + "': " +
            std::to_string(candidates.size()) +
            " complete model directories found — keep exactly one:" + listing);
    }
    return utils::path_to_utf8(candidates.front());
}

json classify(const OperationHookContext& context, const json& request) {
    std::string text;
    if (request.contains("text") && request["text"].is_string()) {
        text = request["text"].get<std::string>();
    } else if (request.contains("input") && request["input"].is_string()) {
        text = request["input"].get<std::string>();
    } else {
        return json{
            {"error", {
                {"message", "Missing 'input' (or 'text') string in classify request"},
                {"type", "invalid_request_error"},
                {"status_code", 400},
            }}
        };
    }
    json body = {{"text", text}};
    if (request.contains("top_k")) body["top_k"] = request["top_k"];
    return context.post_json("/classify", body, 120);
}

std::string startup_error(const std::string& executable, const std::vector<std::string>& args) {
    std::string captured;
    int exit_code = -1;
    try {
        exit_code = utils::ProcessManager::run_process_with_output(
            executable, args,
            [&captured](const std::string& line) {
                if (line.find_first_not_of(" \t\r\n") == std::string::npos) {
                    return true;
                }
                if (!captured.empty()) captured += " | ";
                captured += line;
                return captured.size() < 400;
            },
            "", 20);
    } catch (const std::exception&) {
    }

    if (captured.empty()) {
        std::ostringstream detail;
        detail << "exited with code " << exit_code;
        if (exit_code == static_cast<int>(0xC0000135)) {
            detail << " (STATUS_DLL_NOT_FOUND — a required library is missing "
                      "from the backend install directory)";
        }
        std::error_code ec;
        fs::path bin_dir = utils::path_from_utf8(executable).parent_path();
        std::string listing;
        for (const auto& entry : fs::directory_iterator(bin_dir, ec)) {
            if (ec) break;
            if (!listing.empty()) listing += ", ";
            listing += utils::path_to_utf8(entry.path().filename());
        }
        if (!listing.empty()) detail << "; installed: " << listing;
        captured = detail.str();
    }
    const size_t start = captured.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    captured = captured.substr(start);
    captured.erase(captured.find_last_not_of(" \t\r\n") + 1);
    if (captured.size() > 400) captured = captured.substr(0, 400) + "...";
    return captured;
}

}

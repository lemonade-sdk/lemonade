#include "lemon/model_manager.h"
#include "lemon/model_types.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace lemon {
namespace {

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string normalize_capability(std::string value) {
    value = lower_ascii(std::move(value));
    std::replace(value.begin(), value.end(), '_', '-');
    if (value == "llm") return "chat";
    if (value == "embeddings") return "embedding";
    if (value == "edit") return "image-edit";
    if (value == "3d" || value == "mesh" || value == "model-3d") return "model3d";

    static const std::set<std::string> kCapabilities = {
        "chat", "router", "omni", "image", "image-edit", "transcription",
        "audio-generation", "tts", "model3d", "embedding", "reranking",
        "classification",
    };
    return kCapabilities.count(value) ? value : std::string();
}

size_t decode_cursor(const std::string& cursor) {
    if (cursor.empty()) return 0;
    constexpr const char* kPrefix = "offset:";
    if (cursor.rfind(kPrefix, 0) != 0 || cursor.size() == 7) {
        throw std::invalid_argument("Invalid model query cursor");
    }
    const std::string digits = cursor.substr(7);
    if (!std::all_of(digits.begin(), digits.end(), [](unsigned char ch) {
            return std::isdigit(ch) != 0;
        })) {
        throw std::invalid_argument("Invalid model query cursor");
    }
    try {
        const unsigned long long value = std::stoull(digits);
        if (value > static_cast<unsigned long long>(std::numeric_limits<size_t>::max())) {
            throw std::invalid_argument("Invalid model query cursor");
        }
        return static_cast<size_t>(value);
    } catch (const std::exception&) {
        throw std::invalid_argument("Invalid model query cursor");
    }
}

}  // namespace

std::string ModelManager::model_capability(const ModelInfo& info) {
    // Collections have their own deployment mode, independent of their children.
    if (is_router_collection_recipe(info.recipe)) return "router";
    if (is_omni_collection_recipe(info.recipe)) return "omni";

    switch (info.type) {
        case ModelType::LLM: return "chat";
        case ModelType::EMBEDDING: return "embedding";
        case ModelType::RERANKING: return "reranking";
        case ModelType::TRANSCRIPTION: return "transcription";
        case ModelType::IMAGE: return "image";
        case ModelType::TTS: return "tts";
        case ModelType::AUDIO_GENERATION: return "audio-generation";
        case ModelType::CLASSIFICATION: return "classification";
        case ModelType::MESH: return "model3d";
    }
    return {};
}

std::vector<std::string> ModelManager::model_capabilities(const ModelInfo& info) {
    std::vector<std::string> result;
    auto add = [&result](const std::string& capability) {
        if (!capability.empty() &&
            std::find(result.begin(), result.end(), capability) == result.end()) {
            result.push_back(capability);
        }
    };
    const std::string primary = model_capability(info);
    add(primary);
    for (const auto& label : info.labels) {
        // Deployment type inference recognizes lowercase labels only. Apply the
        // same rule here so "IMAGE" cannot advertise image support on an LLM.
        if (label != lower_ascii(label)) continue;
        // An edit label on a text model must not advertise image editing.
        // Only an explicitly labeled image model supports image-edit.
        const std::string normalized = normalize_capability(label);
        if (normalized == "image-edit" && primary != "image") continue;
        add(normalized);
    }
    return result;
}

ModelQueryResult ModelManager::query_models(const ModelQueryOptions& options) {
    if (options.limit && *options.limit == 0) {
        throw std::invalid_argument("Model query limit must be greater than zero");
    }
    const std::string capability = normalize_capability(options.capability);
    if (!options.capability.empty() && capability.empty()) {
        throw std::invalid_argument("Unknown model capability: " + options.capability);
    }

    std::vector<std::string> tokens;
    std::istringstream words(lower_ascii(options.query));
    for (std::string token; words >> token;) tokens.push_back(std::move(token));

    // The store supplies public IDs and a supported, published model snapshot.
    // We query this, NOT the raw registry, and do not project aliases here.
    std::map<std::string, ModelInfo> candidates = options.downloaded && *options.downloaded
        ? get_downloaded_models() : get_supported_models();
    std::vector<std::pair<std::string, ModelInfo>> matches;
    for (auto& [model_id, info] : candidates) {
        if (options.downloaded && info.downloaded != *options.downloaded) continue;
        if (!capability.empty()) {
            const auto available = model_capabilities(info);
            if (std::find(available.begin(), available.end(), capability) == available.end()) {
                continue;
            }
        }
        if (!tokens.empty()) {
            std::string text = model_id + " " + info.recipe;
            for (const auto& [role, checkpoint] : info.checkpoints) {
                (void)role;
                text += " " + checkpoint;
            }
            for (const auto& label : info.labels) text += " " + label;
            text = lower_ascii(std::move(text));
            if (!std::all_of(tokens.begin(), tokens.end(), [&text](const std::string& token) {
                    return text.find(token) != std::string::npos;
                })) {
                continue;
            }
        }
        matches.emplace_back(model_id, std::move(info));
    }

    ModelQueryResult page;
    page.matching_total = matches.size();
    const size_t begin = decode_cursor(options.cursor);
    if (begin >= matches.size()) return page;

    size_t end = matches.size();
    if (options.limit) {
        end = begin + std::min(*options.limit, matches.size() - begin);
    }
    for (size_t i = begin; i < end; ++i) {
        page.models.push_back(std::move(matches[i]));
    }
    if (end < matches.size()) page.next_cursor = "offset:" + std::to_string(end);
    return page;
}

}  // namespace lemon

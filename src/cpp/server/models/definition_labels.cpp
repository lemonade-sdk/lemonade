#include <lemon/models/definition_labels.h>

#include <lemon/backends/backend_descriptor_registry.h>
#include <lemon/model_types.h>

#include <string>
#include <vector>

namespace lemon {
namespace models {

std::string describe_illegal_labels(const std::string& model_name, const std::string& reason) {
    return "Model '" + model_name + "': " + reason;
}

std::set<std::string> normalized_definition_labels(const nlohmann::json& model_data,
                                                  std::string* illegal) {
    const std::string recipe = model_data.value("recipe", std::string());
    std::vector<std::string> labels = {"custom"};
    for (const auto& label : model_data.value("labels", std::vector<std::string>{})) {
        add_label_once(labels, label);
    }
    if (model_data.value("reasoning", false)) add_label_once(labels, "reasoning");
    if (model_data.value("vision", false)) add_label_once(labels, "vision");
    if (model_data.value("embedding", false)) add_label_once(labels, "embeddings");
    if (model_data.value("reranking", false)) add_label_once(labels, "reranking");

    if (illegal != nullptr) {
        *illegal = backends::illegal_deployment_labels(labels, recipe);
    }
    backends::ensure_deployment_label(labels, recipe);
    return std::set<std::string>(labels.begin(), labels.end());
}

}  // namespace models
}  // namespace lemon

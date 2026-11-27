#include "lemon/backends/fastflowlm/flm_model_labels.h"

#include <algorithm>
#include <cctype>

#include "lemon/model_types.h"

namespace lemon {
namespace backends {
namespace fastflowlm {

void add_flm_mode_labels(const std::string& checkpoint, std::vector<std::string>& labels) {
    // FLM's decision models need the SystemOne deployment labels. Other
    // explicit deployment modes (including R2T2's "transcription") must be
    // preserved. Older catalogs may omit the mode, so retain checkpoint-name
    // fallbacks for Whisper and embedding models; the rest are chat models.
    // "chat-transcription" is an input modality, not standalone ASR.
    if (std::find(labels.begin(), labels.end(), "decision") != labels.end()) {
        add_label_once(labels, "classification");
        add_label_once(labels, "systemone");
        return;
    }
    ModelType type;
    if (find_deployment_mode(labels, type)) {
        return;
    }
    std::string id = checkpoint;
    std::transform(id.begin(), id.end(), id.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (id.find("whisper") != std::string::npos) {
        add_label_once(labels, "transcription");
    } else if (id.find("embed") != std::string::npos) {
        add_label_once(labels, "embeddings");
    } else {
        add_label_once(labels, "chat");
    }
}

} // namespace fastflowlm
} // namespace backends
} // namespace lemon

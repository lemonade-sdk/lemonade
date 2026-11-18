#include "lemon/backends/fastflowlm/flm_model_labels.h"

#include <algorithm>
#include <cctype>

#include "lemon/model_types.h"

namespace lemon {
namespace backends {
namespace fastflowlm {

void add_flm_mode_labels(const std::string& checkpoint, std::vector<std::string>& labels) {
    // `flm list --json` reports characteristics and input modalities, never a
    // deployment mode: FLM's chat models report no label at all (llama3.2:3b,
    // lfm2:1.2b, ...) or only "reasoning"/"vision"/"tool-calling", and the
    // any-to-text ones add "audio"/"chat-transcription". The d3 models say
    // "decision", which `flm serve --decision` serves on /v1/systemone. The
    // catalog's only other non-chat models name their mode ("embeddings" on
    // embed-gemma:300m, "transcription" on whisper-v3:turbo) and both are
    // identifiable from the checkpoint name, so classify from the name and
    // treat the rest as chat.
    // https://github.com/ROCm/FastFlowLM/issues/668 would let FLM report the
    // mode and remove this workaround.
    if (std::find(labels.begin(), labels.end(), "decision") != labels.end()) {
        add_label_once(labels, "classification");
        add_label_once(labels, "systemone");
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

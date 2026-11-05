#pragma once

#include <string>
#include <vector>

namespace lemon {
namespace backends {
namespace fastflowlm {

// Adds the deployment-mode label of an FLM catalog model to the labels that
// `flm list --json` reports for it: `classification` and `systemone` for a
// `decision` model, `transcription` or `embeddings` when the checkpoint names
// one, and `chat` otherwise.
void add_flm_mode_labels(const std::string& checkpoint, std::vector<std::string>& labels);

} // namespace fastflowlm
} // namespace backends
} // namespace lemon

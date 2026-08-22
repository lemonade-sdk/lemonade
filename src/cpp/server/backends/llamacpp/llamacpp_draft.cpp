#include "lemon/backends/llamacpp/llamacpp_draft.h"

#include <algorithm>
#include <cctype>

namespace lemon {
namespace backends {

bool is_dflash_draft_checkpoint(const std::string& checkpoint) {
    std::string lowered = checkpoint;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const size_t separator = lowered.find_last_of("/:\\");
    const std::string filename =
        separator == std::string::npos ? lowered : lowered.substr(separator + 1);
    return filename.rfind("dflash-", 0) == 0 || filename == "dflash.gguf";
}

namespace {
bool has_label(const std::vector<std::string>& labels, const std::string& label) {
    return std::find(labels.begin(), labels.end(), label) != labels.end();
}
} // namespace

DraftActivation compute_draft_activation(const std::vector<std::string>& labels,
                                         const std::string& draft_checkpoint,
                                         bool draft_file_present) {
    DraftActivation act;
    if (!draft_file_present) {
        // No drafter on disk: the base model runs without speculative decoding.
        return act;
    }

    const bool has_dflash_label = has_label(labels, "dflash");
    const bool has_mtp_label = has_label(labels, "mtp");
    const bool is_dflash_draft = is_dflash_draft_checkpoint(draft_checkpoint);

    if (is_dflash_draft && has_dflash_label) {
        act.use_draft_checkpoint = true;
        act.spec_type = "draft-dflash";
    } else if (!is_dflash_draft && has_mtp_label) {
        act.use_draft_checkpoint = true;
        act.spec_type = "draft-mtp";
    }

    return act;
}

} // namespace backends
} // namespace lemon

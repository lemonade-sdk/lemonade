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
                                         bool draft_file_present,
                                         const std::string& architecture,
                                         const std::string& mmproj_path,
                                         bool hf_load) {
    DraftActivation act;

    const bool has_dflash_label = has_label(labels, "dflash");
    const bool has_mtp_label = has_label(labels, "mtp");
    const bool is_dflash_draft = is_dflash_draft_checkpoint(draft_checkpoint);
    const bool has_external_draft = !draft_checkpoint.empty() && draft_file_present;

    if (is_dflash_draft) {
        // DFlash always needs its external drafter file on disk.
        if (has_dflash_label && has_external_draft) {
            act.use_draft_checkpoint = true;
            act.spec_type = "draft-dflash";
        }
        return act;
    }

    if (!has_mtp_label) {
        return act;
    }

    // A configured external draft that is missing on disk: run the base model
    // without speculative decoding rather than fail the launch.
    if (!draft_checkpoint.empty() && !draft_file_present) {
        return act;
    }

    // GLM4-MOE / ChatGLM4: the draft graph builder accesses vision tensors that
    // only exist when an mmproj companion is present. Without one, MTP
    // speculative decoding crashes llama-server (see #2451) — embedded MTP
    // included. Keep MTP inactive unless mmproj resolves or the model loads via
    // -hf (llama-server resolves the companion itself).
    const std::string arch_lower = [&]() {
        std::string s = architecture;
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }();
    const bool is_glm4_moe =
        arch_lower.find("glm4") != std::string::npos
        || arch_lower.find("chatglm4") != std::string::npos;
    if (is_glm4_moe && mmproj_path.empty() && !hf_load) {
        return act;
    }

    // MTP activates. With an external drafter on disk emit --model-draft as
    // well; an `mtp` label without one means the MTP weights are embedded in
    // the main GGUF, so --spec-type draft-mtp alone is enough.
    act.use_draft_checkpoint = has_external_draft;
    act.spec_type = "draft-mtp";
    return act;
}

} // namespace backends
} // namespace lemon

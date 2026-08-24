#pragma once

#include <string>
#include <vector>

namespace lemon {
namespace backends {

// Decision about whether/how to activate an optional `draft` (MTP / DFlash)
// checkpoint when launching llama-server.
struct DraftActivation {
    // Emit --model-draft <draft_path> when true.
    bool use_draft_checkpoint = false;
    // Emit --spec-type <spec_type> when non-empty ("draft-mtp" or "draft-dflash").
    std::string spec_type;
};

// True when `checkpoint` names a DFlash drafter ("dflash.gguf" or "dflash-*").
bool is_dflash_draft_checkpoint(const std::string& checkpoint);

// Compute draft activation from the model's labels, the raw draft checkpoint
// reference, whether the resolved draft file exists on disk, the GGUF
// architecture, the mmproj path, and hf_load.
//
// Registration and activation are kept separate: the `draft` checkpoint only
// describes where the drafter lives, while the `mtp` / `dflash` label is the
// switch that enables speculative decoding. An `mtp` label without an external
// drafter means the MTP weights are embedded in the main GGUF: emit
// --spec-type draft-mtp without --model-draft. GLM4-MOE embedded MTP crashes
// without an mmproj companion (see #2451).
DraftActivation compute_draft_activation(const std::vector<std::string>& labels,
                                         const std::string& draft_checkpoint,
                                         bool draft_file_present,
                                         const std::string& architecture,
                                         const std::string& mmproj_path,
                                         bool hf_load);

} // namespace backends
} // namespace lemon

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
// reference, and whether the resolved draft file actually exists on disk.
//
// Registration and activation are kept separate: the `draft` checkpoint only
// describes where the drafter lives, while the `mtp` / `dflash` label is the
// switch that enables speculative decoding. A drafter present on disk without
// its matching label does not activate anything, and a label without a present
// drafter also stays inactive (the base model runs normally).
DraftActivation compute_draft_activation(const std::vector<std::string>& labels,
                                         const std::string& draft_checkpoint,
                                         bool draft_file_present);

} // namespace backends
} // namespace lemon

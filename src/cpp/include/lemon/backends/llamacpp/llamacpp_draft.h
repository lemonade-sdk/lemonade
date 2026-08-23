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
// reference, whether the resolved draft file actually exists on disk, and the
// GGUF architecture (used to gate GLM4-MOE, see below).
//
// Registration and activation are kept separate: the `draft` checkpoint only
// describes where the drafter lives, while the `mtp` / `dflash` label is the
// switch that enables speculative decoding. A drafter present on disk without
// its matching label does not activate anything, and a label without a present
// drafter also stays inactive (the base model runs normally).
//
// GLM4-MOE / ChatGLM4 models embed MTP layers in the GGUF but their draft
// context construction accesses vision tensors that only exist when an mmproj
// file is present; without one, MTP speculative decoding crashes llama-server
// (see #2451). For those architectures MTP stays inactive unless the mmproj
// path resolves (or the model is loaded via -hf, where llama-server resolves
// the companion itself). Text-only MTP models (Gemma-4, Qwen3.x) are unaffected.
DraftActivation compute_draft_activation(const std::vector<std::string>& labels,
                                         const std::string& draft_checkpoint,
                                         bool draft_file_present,
                                         const std::string& architecture,
                                         const std::string& mmproj_path,
                                         bool hf_load);

} // namespace backends
} // namespace lemon

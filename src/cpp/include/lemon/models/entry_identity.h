#pragma once

#include <lemon/canonical_id.h>

#include <map>
#include <string>

namespace lemon {

struct ModelInfo;

namespace models {

// What one store entry is, what its bare name is, and whether its key already
// says where it came from.
struct EntryIdentity {
    ModelSource source = ModelSource::Builtin;
    std::string bare;
    // A provider listing qualifies its own ids — `fireworks.kimi-k2p5` — which is
    // none of the handle prefixes. Such an entry needs no alias and must never be
    // re-prefixed: that is how a cloud model listed as `builtin.fireworks.X`.
    bool qualifies_itself = false;
};

EntryIdentity entry_identity(const std::string& cache_key, const ModelInfo& info);

// Lower wins a contested public name. A model the user configured a provider for
// outranks a file somebody left in a scanned directory.
int entry_precedence_rank(const EntryIdentity& identity);

struct ModelAliasMaps {
    std::map<std::string, std::string> public_model_aliases;
    std::map<std::string, std::string> canonical_public_names;
};

ModelAliasMaps compute_model_alias_maps(const std::map<std::string, ModelInfo>& models);

}  // namespace models
}  // namespace lemon

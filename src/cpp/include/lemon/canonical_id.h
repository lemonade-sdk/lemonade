#pragma once

#include <optional>
#include <string>

namespace lemon {

// ----------------------------------------------------------------------------
// Canonical model IDs
// ----------------------------------------------------------------------------
//
// Every model has a canonical ID of the form `<source>.<bare-name>`:
//   user.NAME    — model registered via `lemonade pull` (entry in user_models.json)
//   extra.NAME   — model discovered in --extra-models-dir
//   builtin.NAME — model compiled into server_models.json
//
// `/v1/models` and Ollama `/api/tags` emit the bare name when a model is the
// precedence-winner for its bare name (precedence: Registered > Imported >
// Builtin), or the canonical-prefixed ID when shadowed. Bare names are accepted
// as input anywhere a model name is accepted and resolve to the winner.
//
// Display strings like "NAME (registered)" / "NAME (imported)" / "NAME (builtin)"
// are a GUI concern only. The C++ server emits canonical IDs only; the Tauri
// renderer applies the (registered)/(imported)/(builtin) suffix transform.

enum class ModelSource { Registered, Imported, Builtin };

struct CanonicalId {
    ModelSource source;
    std::string bare_name;
    std::string full() const;
};

inline const char* canonical_prefix(ModelSource source) {
    switch (source) {
        case ModelSource::Registered: return "user.";
        case ModelSource::Imported:   return "extra.";
        case ModelSource::Builtin:    return "builtin.";
    }
    return "";
}

inline std::string canonical_id(ModelSource source, const std::string& bare_name) {
    return std::string(canonical_prefix(source)) + bare_name;
}

inline std::string CanonicalId::full() const {
    return canonical_id(source, bare_name);
}

inline std::optional<CanonicalId> parse_canonical_id(const std::string& id) {
    static constexpr const char USER_PREFIX[] = "user.";
    static constexpr const char EXTRA_PREFIX[] = "extra.";
    static constexpr const char BUILTIN_PREFIX[] = "builtin.";
    if (id.rfind(USER_PREFIX, 0) == 0) {
        return CanonicalId{ModelSource::Registered, id.substr(sizeof(USER_PREFIX) - 1)};
    }
    if (id.rfind(EXTRA_PREFIX, 0) == 0) {
        return CanonicalId{ModelSource::Imported, id.substr(sizeof(EXTRA_PREFIX) - 1)};
    }
    if (id.rfind(BUILTIN_PREFIX, 0) == 0) {
        return CanonicalId{ModelSource::Builtin, id.substr(sizeof(BUILTIN_PREFIX) - 1)};
    }
    return std::nullopt;
}

inline int precedence_rank(ModelSource source) {
    switch (source) {
        case ModelSource::Registered: return 0;
        case ModelSource::Imported:   return 1;
        case ModelSource::Builtin:    return 2;
    }
    return 99;
}

// Only Registered (user.*) is creatable via the pull/import flows. Imported
// (extra.*) is discovered from disk and Builtin (builtin.*) ships in the
// binary, so both prefixes are reserved when accepting new-model registrations.
inline bool is_reserved_for_registration(ModelSource source) {
    return source == ModelSource::Imported || source == ModelSource::Builtin;
}

// Reject a registration name when its source token is reserved, OR when it's a
// user.* alias whose bare-name part itself begins with a reserved source token
// (e.g. user.builtin.Foo, user.extra.Foo). The latter would otherwise hijack
// the builtin.<bare> / extra.<bare> alias slot in the public-alias map and
// break the spec's "builtin.X always means built-in X" contract.
inline bool is_reserved_registration_name(const std::string& model_name) {
    auto canon = parse_canonical_id(model_name);
    if (!canon) {
        return false;
    }
    if (is_reserved_for_registration(canon->source)) {
        return true;
    }
    if (canon->source == ModelSource::Registered) {
        if (auto inner = parse_canonical_id(canon->bare_name);
            inner && is_reserved_for_registration(inner->source)) {
            return true;
        }
    }
    return false;
}

} // namespace lemon

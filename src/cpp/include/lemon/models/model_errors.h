#pragma once

#include <stdexcept>

namespace lemon {

// Thrown by ModelManager::download_model when a pull request names a model
// that (a) is not registered, (b) is not in the filtered-out registry, and
// (c) lacks the `user.` prefix that would make it a new-model registration
// attempt.
//
// CONTRACT: the /pull HTTP handler catches this type and attaches
// {"code": kUnknownModelErrorCode, ...} to the error response. The lemonade
// CLI keys off that code to replace the message with a friendlier one that
// points at `lemonade list` and `lemonade pull CHECKPOINT`. The CLI inlines
// the "unknown_model" literal to avoid pulling this server header into the
// CLI; update cli/lemonade_client.cpp in lockstep if this constant changes.
class UnknownModelError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
constexpr const char* kUnknownModelErrorCode = "unknown_model";

// Thrown when a definition names a deployment mode its recipe's backend cannot
// serve. Registering it would list the model as something it is not and then
// fail at inference time. A definition naming *no* mode is not an error: the
// recipe's default is stamped instead.
//
// CONTRACT: the /pull HTTP handler catches this type and returns 400. Loading an
// already-persisted entry does not go through registration, so an entry written
// by an older version is still normalized rather than blocking startup.
class InvalidModelDefinitionError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

}  // namespace lemon

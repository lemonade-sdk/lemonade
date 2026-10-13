#pragma once

#include <lemon/model_registry.h>

#include <optional>
#include <string>

namespace lemon {
namespace models {

// A checkpoint is stored as `repo_id[:variant]` and that grammar is read in
// several places, so it lives here rather than being re-parsed per call site.
std::string repo_id_from_checkpoint(const std::string& checkpoint);
std::string variant_from_checkpoint(const std::string& checkpoint);

enum class LocatorScheme {
    Registry,
    Cache,
    Import,
    Url,
};

// Where a model's bytes come from. It deliberately does not answer where they
// are on disk: resolving an artifact needs backend ops, which a value type
// cannot own.
//
// There is no default constructor on purpose. A default registry would have to
// name one concrete provider inside a generic type, which turns a convenience
// initializer into a dependency every future registry inherits.
struct Locator {
    Locator() = delete;

    static Locator from_checkpoint(const std::string& checkpoint, const std::string& source);

    std::string to_string() const;

    bool is_registry() const { return scheme == LocatorScheme::Registry; }
    bool is_cache() const { return scheme == LocatorScheme::Cache; }
    bool is_import() const { return scheme == LocatorScheme::Import; }
    bool is_url() const { return scheme == LocatorScheme::Url; }

    // Registry locators only.
    std::string repo_id() const { return is_registry() ? path : std::string(); }

    LocatorScheme scheme;
    std::string path;
    std::string variant;

    // Set exactly when the scheme is Registry, and unset for every other
    // scheme rather than defaulted to whichever provider shipped first.
    std::optional<RemoteRegistrySource> registry;
    std::string revision;

  private:
    Locator(LocatorScheme scheme, std::string path, std::string variant,
            std::optional<RemoteRegistrySource> registry, std::string revision);
};

}  // namespace models
}  // namespace lemon

#pragma once

#include <lemon/model_registry.h>

#include <optional>
#include <string>

namespace lemon {
namespace models {

// The `repo_id[:variant]` grammar, read in several places.
std::string repo_id_from_checkpoint(const std::string& checkpoint);
std::string variant_from_checkpoint(const std::string& checkpoint);

enum class LocatorScheme {
    Registry,
    Cache,
    Import,
    Url,
};

// Where a model's bytes come from, not where they sit on disk; no defaulted host.
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

// Set only for the Registry scheme; never defaulted to a provider.
    std::optional<RegistryId> registry;
    std::string revision;

  private:
    Locator(LocatorScheme scheme, std::string path, std::string variant,
            std::optional<RegistryId> registry, std::string revision);
};

}  // namespace models
}  // namespace lemon

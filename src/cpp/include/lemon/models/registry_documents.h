#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace lemon {
namespace models {

enum class RegistryDocument {
    Builtin,  // shipped in resources
    User,     // written by registrations
};

// The two registry documents, published immutably: a reader takes no lock and
// cannot see a half-applied change.
//
// A writer re-reads the file instead of editing the document it holds. Two
// registrations that started from one document would otherwise land a file
// naming only the later model, and the first would read back missing.
//
// write_mutex_ orders writers and the publish; this class calls out to nothing,
// so that lock is never held against another component's.
class RegistryDocuments {
  public:
    using json = nlohmann::json;

    // A consistent pair; a row pointer stays valid while the pin lives.
    struct Pin {
        std::shared_ptr<const json> builtin;
        std::shared_ptr<const json> user;

        bool contains(RegistryDocument document, const std::string& bare_name) const;

        // Null when no row has that key.
        const json* row(RegistryDocument document, const std::string& bare_name) const;
    };

    // A user file that is absent or unreadable is an empty registry.
    RegistryDocuments(json builtin, std::string user_file);

    Pin pin() const;

    // Read-modify-write of the user file: the mutation sees the copy on disk and
    // says whether it changed anything, which is what decides the write.
    using Mutation = std::function<bool(json& user_document)>;
    bool update_user(const Mutation& mutation);

  private:
    // Declared first: the constructor loads user_ from it.
    std::string user_file_;
    std::shared_ptr<const json> builtin_;
    std::shared_ptr<const json> user_;
    std::mutex write_mutex_;
};

}  // namespace models
}  // namespace lemon

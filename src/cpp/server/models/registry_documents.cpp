#include <lemon/models/registry_documents.h>

#include <lemon/utils/json_file.h>

namespace lemon {
namespace models {

using json = nlohmann::json;

namespace {

const json* document_of(const RegistryDocuments::Pin& pin, RegistryDocument document) {
    return document == RegistryDocument::User ? pin.user.get() : pin.builtin.get();
}

// A file that is not a set of rows is not this class's property to keep.
json document_or_empty(json document) {
    return document.is_object() ? document : json::object();
}

}  // namespace

bool RegistryDocuments::Pin::contains(RegistryDocument document,
                                      const std::string& bare_name) const {
    const json* doc = document_of(*this, document);
    return doc->is_object() && doc->contains(bare_name);
}

const nlohmann::json* RegistryDocuments::Pin::row(RegistryDocument document,
                                                 const std::string& bare_name) const {
    const json* doc = document_of(*this, document);
    if (!doc->is_object()) {
        return nullptr;
    }
    const auto found = doc->find(bare_name);
    return found == doc->end() ? nullptr : &(*found);
}

RegistryDocuments::RegistryDocuments(json builtin, std::string user_file)
    : user_file_(std::move(user_file)),
      builtin_(std::make_shared<const json>(std::move(builtin))),
      user_(std::make_shared<const json>(
          document_or_empty(utils::load_optional_json(user_file_)))) {}

RegistryDocuments::Pin RegistryDocuments::pin() const {
    return Pin{std::atomic_load(&builtin_), std::atomic_load(&user_)};
}

bool RegistryDocuments::update_user(const Mutation& mutation) {
    std::lock_guard<std::mutex> lock(write_mutex_);

    json document = document_or_empty(utils::load_optional_json(user_file_));
    if (!mutation(document)) {
        return false;
    }

    utils::save_json_atomic(user_file_, document);
    std::atomic_store(&user_, std::make_shared<const json>(std::move(document)));
    return true;
}

}  // namespace models
}  // namespace lemon

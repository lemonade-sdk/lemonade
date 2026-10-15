#include <lemon/models/catalog_set.h>

#include <algorithm>

namespace lemon {
namespace models {

void CatalogSet::add(std::shared_ptr<ModelCatalog> catalog) {
    if (!catalog) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string id = catalog->descriptor().id;
    for (auto& held : entries_) {
        if (held->descriptor().id == id) {
            held = std::move(catalog);
            order_locked();
            return;
        }
    }
    entries_.push_back(std::move(catalog));
    order_locked();
}

bool CatalogSet::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto kept = std::remove_if(entries_.begin(), entries_.end(),
                                    [&id](const std::shared_ptr<ModelCatalog>& held) {
                                        return held->descriptor().id == id;
                                    });
    if (kept == entries_.end()) {
        return false;
    }
    entries_.erase(kept, entries_.end());
    return true;
}

std::shared_ptr<ModelCatalog> CatalogSet::find(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& held : entries_) {
        if (held->descriptor().id == id) {
            return held;
        }
    }
    return nullptr;
}

size_t CatalogSet::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

bool CatalogSet::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.empty();
}

std::vector<std::shared_ptr<ModelCatalog>> CatalogSet::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<ModelCatalog>> catalogs;
    catalogs.reserve(entries_.size());
    for (const auto& held : entries_) {
        catalogs.push_back(held);
    }
    return catalogs;
}

std::vector<CatalogDescriptor> CatalogSet::descriptors() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CatalogDescriptor> descriptors;
    descriptors.reserve(entries_.size());
    for (const auto& held : entries_) {
        descriptors.push_back(held->descriptor());
    }
    return descriptors;
}

void CatalogSet::replace_with(std::vector<std::shared_ptr<ModelCatalog>> catalogs) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    for (auto& catalog : catalogs) {
        if (catalog) {
            entries_.push_back(std::move(catalog));
        }
    }
    order_locked();
}

bool CatalogSet::before(const std::shared_ptr<ModelCatalog>& left,
                        const std::shared_ptr<ModelCatalog>& right) {
    return left->descriptor().rank < right->descriptor().rank;
}

void CatalogSet::order_locked() {
    std::stable_sort(entries_.begin(), entries_.end(), &CatalogSet::before);
}

}  // namespace models
}  // namespace lemon

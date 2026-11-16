#include "lemon/models/catalog_fetch_clock.h"

namespace lemon {
namespace models {

void CatalogFetchClock::mark_fetched(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    fetched_at_[id] = std::chrono::steady_clock::now();
}

bool CatalogFetchClock::fresh(const std::string& id, std::chrono::seconds window) const {
    if (window.count() <= 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = fetched_at_.find(id);
    if (found == fetched_at_.end()) {
        return false;
    }
    return std::chrono::steady_clock::now() - found->second < window;
}

void CatalogFetchClock::forget_all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    fetched_at_.clear();
}

}  // namespace models
}  // namespace lemon

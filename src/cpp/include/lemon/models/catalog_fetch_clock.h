#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <string>

namespace lemon {
namespace models {

// Which catalog bodies this process fetched, and when. A store build constructs
// every catalog from scratch, so this is the only thing that survives one.
// In-memory deliberately: a restart asks the origin once.
class CatalogFetchClock {
  public:
    void mark_fetched(const std::string& id) const;

    bool fresh(const std::string& id, std::chrono::seconds window) const;

    void forget_all() const;

  private:
    mutable std::mutex mutex_;
    mutable std::map<std::string, std::chrono::steady_clock::time_point> fetched_at_;
};

}  // namespace models
}  // namespace lemon

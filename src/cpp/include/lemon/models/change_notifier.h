#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace lemon {
namespace models {

// Two signals, one subscriber each, registered at startup. A subscriber runs
// outside every store lock, after the snapshot for its generation is published.
class ModelChangeNotifier {
  public:
    // Recomputes which helper models the policies in force still need.
    void on_models_changed(std::function<void(std::uint64_t)> subscriber);

    // Unloads the model when it is loaded, so the next request opens new bytes.
    void on_model_updated(std::function<void(const std::string&)> subscriber);

    void models_changed(std::uint64_t generation);
    void model_updated(const std::string& canonical_id);

  private:
    std::mutex mutex_;
    std::function<void(std::uint64_t)> models_changed_;
    std::function<void(const std::string&)> model_updated_;
};

}  // namespace models
}  // namespace lemon

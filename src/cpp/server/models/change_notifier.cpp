#include <lemon/models/change_notifier.h>

#include <lemon/utils/aixlog.hpp>

namespace lemon {
namespace models {

namespace {

// delete_model fires from a scope-guard destructor, where a propagating
// exception would terminate.
void swallow(const std::function<void()>& fire, const char* what) {
    try {
        fire();
    } catch (const std::exception& e) {
        LOG(WARNING, "ModelManager") << what << " subscriber threw: " << e.what() << std::endl;
    } catch (...) {
        LOG(WARNING, "ModelManager") << what << " subscriber threw a non-standard exception"
                                     << std::endl;
    }
}

}  // namespace

void ModelChangeNotifier::on_models_changed(std::function<void(std::uint64_t)> subscriber) {
    std::lock_guard<std::mutex> lock(mutex_);
    models_changed_ = std::move(subscriber);
}

void ModelChangeNotifier::on_model_updated(std::function<void(const std::string&)> subscriber) {
    std::lock_guard<std::mutex> lock(mutex_);
    model_updated_ = std::move(subscriber);
}

void ModelChangeNotifier::models_changed(std::uint64_t generation) {
    // A subscriber can request another rebuild; re-entry must not re-fire.
    static thread_local bool in_notify = false;
    if (in_notify) {
        return;
    }

    std::function<void(std::uint64_t)> subscriber;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        subscriber = models_changed_;
    }
    if (!subscriber) {
        return;
    }

    in_notify = true;
    struct ResetGuard {
        ~ResetGuard() { in_notify = false; }
    } reset_guard;

    swallow([&] { subscriber(generation); }, "models-changed");
}

void ModelChangeNotifier::model_updated(const std::string& canonical_id) {
    std::function<void(const std::string&)> subscriber;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        subscriber = model_updated_;
    }
    if (!subscriber) {
        return;
    }

    swallow([&] { subscriber(canonical_id); }, "model-updated");
}

}  // namespace models
}  // namespace lemon

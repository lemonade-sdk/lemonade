#pragma once

#include <memory>
#include <mutex>

namespace lemon {

// Prevents the OS from suspending/idling while inference is active. Refcounted:
// the first acquire() takes the OS-level lock, the matching last release() drops
// it.
//
// Linux: uses systemd-logind Inhibit ("sleep:idle", "block"). If logind is
// unreachable (containers, WSL, minimal environments) the feature degrades to
// no-op and logs debug on first failure (once), then stays silent.
// macOS / Windows / non-systemd builds: no-op.
class SuspendInhibitor {
public:
    virtual ~SuspendInhibitor() = default;

    void acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (refcount_++ == 0) {
            on_first_acquire();
        }
    }

    void release() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (refcount_ > 0 && --refcount_ == 0) {
            on_last_release();
        }
    }

    // Test hook: returns current refcount (thread-safe).
    int refcount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return refcount_;
    }

protected:
    // Called exactly when refcount transitions 0->1.
    virtual void on_first_acquire() {}
    // Called exactly when refcount transitions 1->0.
    virtual void on_last_release() {}

private:
    mutable std::mutex mutex_;
    int refcount_ = 0;
};

std::unique_ptr<SuspendInhibitor> create_suspend_inhibitor();

} // namespace lemon

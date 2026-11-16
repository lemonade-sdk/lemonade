#pragma once

#include <atomic>
#include <functional>

namespace lemon {
namespace utils {

// The one cancellation currency. A flag for a caller that owns the state, a
// predicate for a caller that only has a question to ask; both optional, and
// any transport that can abort consults both.
struct CancelToken {
    std::atomic<bool>* flag = nullptr;
    std::function<bool()> should_cancel = nullptr;

    bool cancelled() const {
        return (flag != nullptr && flag->load()) ||
               (should_cancel != nullptr && should_cancel());
    }
};

}  // namespace utils
}  // namespace lemon

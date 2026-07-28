#include <lemon/suspend_inhibitor.h>

namespace lemon {

namespace {
class NoopSuspendInhibitor : public SuspendInhibitor {
public:
    ~NoopSuspendInhibitor() override = default;
};
} // namespace

std::unique_ptr<SuspendInhibitor> create_suspend_inhibitor() {
    return std::make_unique<NoopSuspendInhibitor>();
}

} // namespace lemon

// Standalone test for SuspendInhibitor refcounting logic.
//
// Exercises the production refcounting path in the base class (acquire/release,
// mutex-protected refcount, underflow guard) by subclassing SuspendInhibitor and
// overriding on_first_acquire()/on_last_release() to track hook calls. The Linux
// logind acquisition and OS-level fd management live in suspend_linux.cpp; this
// test validates the shared refcounting mechanism that both LinuxSuspendInhibitor
// and NoopSuspendInhibitor inherit.

#include <lemon/suspend_inhibitor.h>
#include <cstdio>

using namespace lemon;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

// Track calls to the base-class hooks without duplicating refcount logic.
class TrackingInhibitor : public SuspendInhibitor {
public:
    void on_first_acquire() override { ++lock_count_; }
    void on_last_release() override { ++unlock_count_; }

    // Only called from single-threaded tests.
    int lock_count() { return lock_count_; }
    int unlock_count() { return unlock_count_; }

private:
    int lock_count_ = 0;
    int unlock_count_ = 0;
};

static void test_single_acquire_release() {
    TrackingInhibitor inhibitor;
    inhibitor.acquire();
    check("single acquire: refcount=1", inhibitor.refcount() == 1);
    check("single acquire: on_first_acquire called once", inhibitor.lock_count() == 1);
    check("single acquire: on_last_release not called", inhibitor.unlock_count() == 0);

    inhibitor.release();
    check("single release: refcount=0", inhibitor.refcount() == 0);
    check("single release: on_last_release called once", inhibitor.unlock_count() == 1);
}

static void test_nested_acquire_release() {
    TrackingInhibitor inhibitor;

    inhibitor.acquire();  // 0->1, on_first_acquire
    inhibitor.acquire();  // 1->2, no hook
    inhibitor.acquire();  // 2->3, no hook

    check("nested acquire: refcount=3", inhibitor.refcount() == 3);
    check("nested acquire: on_first_acquire once", inhibitor.lock_count() == 1);
    check("nested acquire: on_last_release not called", inhibitor.unlock_count() == 0);

    inhibitor.release();  // 3->2, no hook
    check("first release: refcount=2", inhibitor.refcount() == 2);
    check("first release: on_last_release not called", inhibitor.unlock_count() == 0);

    inhibitor.release();  // 2->1, no hook
    check("second release: refcount=1", inhibitor.refcount() == 1);
    check("second release: on_last_release not called", inhibitor.unlock_count() == 0);

    inhibitor.release();  // 1->0, on_last_release
    check("third release: refcount=0", inhibitor.refcount() == 0);
    check("third release: on_last_release once", inhibitor.unlock_count() == 1);
}

static void test_multiple_cycles() {
    TrackingInhibitor inhibitor;

    // Cycle 1
    inhibitor.acquire();
    inhibitor.acquire();
    inhibitor.release();
    inhibitor.release();

    check("cycle 1: on_first_acquire/on_last_release once",
          inhibitor.lock_count() == 1 && inhibitor.unlock_count() == 1);

    // Cycle 2
    inhibitor.acquire();
    inhibitor.release();

    check("cycle 2: on_first_acquire/on_last_release twice total",
          inhibitor.lock_count() == 2 && inhibitor.unlock_count() == 2);

    // Cycle 3 (nested)
    inhibitor.acquire();
    inhibitor.acquire();
    inhibitor.acquire();
    inhibitor.release();
    inhibitor.release();
    inhibitor.release();

    check("cycle 3: on_first_acquire/on_last_release three times total",
          inhibitor.lock_count() == 3 && inhibitor.unlock_count() == 3);
}

static void test_underflow_protection() {
    TrackingInhibitor inhibitor;

    // Release without acquire should be safe (no hook, refcount stays 0)
    inhibitor.release();
    check("underflow: refcount stays 0", inhibitor.refcount() == 0);
    check("underflow: on_last_release not called", inhibitor.unlock_count() == 0);

    // Now acquire and verify normal behavior
    inhibitor.acquire();
    check("after underflow: acquire works", inhibitor.refcount() == 1);
    check("after underflow: on_first_acquire called once", inhibitor.lock_count() == 1);
}

static void test_factory_creates_valid_instance() {
    auto inhibitor = create_suspend_inhibitor();
    check("factory: returns non-null", inhibitor != nullptr);

    // The real factory returns either LinuxSuspendInhibitor or NoopSuspendInhibitor
    // depending on HAVE_SYSTEMD. Both should be usable without crashing.
    inhibitor->acquire();
    inhibitor->release();
    check("factory: acquire/release don't crash", true);
}

int main() {
    std::printf("\n=== SuspendInhibitor Refcounting Tests ===\n\n");

    test_single_acquire_release();
    test_nested_acquire_release();
    test_multiple_cycles();
    test_underflow_protection();
    test_factory_creates_valid_instance();

    std::printf("\n");
    if (g_failures == 0) {
        std::printf("All tests passed.\n");
        return 0;
    } else {
        std::printf("%d test(s) failed.\n", g_failures);
        return 1;
    }
}

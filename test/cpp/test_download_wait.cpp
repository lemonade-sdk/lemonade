#include "lemon/server/download_manager.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

using lemon::DownloadManager;
using Clock = std::chrono::steady_clock;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

// Two loads need the same backend: the first waits without a cancel flag, the second
// cancels. Only the second waiter stops; the shared install and the first wait go on.
static void test_canceled_waiter_leaves_the_install_running() {
    DownloadManager downloads;
    std::atomic<bool> release{false};
    std::atomic<bool> install_finished{false};
    auto install = [&](lemon::DownloadProgressCallback, lemon::utils::CancelToken) {
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        install_finished.store(true);
    };

    std::atomic<bool> first_done{false};
    std::thread first([&] {
        downloads.run_tracked("backend:test", "backend", "test", install);
        first_done.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::atomic<bool> cancel{false};
    std::string second_error;
    Clock::time_point second_stopped;
    std::thread second([&] {
        try {
            downloads.run_tracked("backend:test", "backend", "test", install, &cancel);
        } catch (const std::runtime_error& e) {
            second_error = e.what();
        }
        second_stopped = Clock::now();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const auto canceled_at = Clock::now();
    cancel.store(true);
    second.join();

    check("wait: the canceled waiter stops with \"load cancelled\"", second_error == "load cancelled");
    check("wait: it stops within a second of the cancel",
          second_stopped - canceled_at < std::chrono::seconds(1));
    check("wait: the shared install is still running", !install_finished.load() && !first_done.load());

    release.store(true);
    first.join();
    check("wait: the install finishes for the waiter that stayed",
          install_finished.load() && first_done.load());
}

int main() {
    test_canceled_waiter_leaves_the_install_running();
    if (g_failures) {
        std::printf("%d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("ALL PASS (0 failures)\n");
    return 0;
}

// The host's singleton and takeover locks.
#include "agent_test_util.h"

#include <atomic>
#include <thread>

#include "lemon/agents/agent_lock.h"

using agent_test::check;
using lemon::agents::FileLock;
namespace fs = std::filesystem;

int main() {
    const fs::path dir = agent_test::temp_dir("agent-lock");
    const fs::path lock_path = dir / "host.lock";

    check("nothing holds a missing lock", !FileLock::is_held(lock_path));
    auto first = FileLock::try_acquire(lock_path);
    check("first holder gets the lock", first != nullptr);
    check("second holder is refused", FileLock::try_acquire(lock_path) == nullptr);
    check("the lock reports held", FileLock::is_held(lock_path));

    // A waiter blocks until the holder lets go.
    std::atomic<bool> acquired{false};
    std::thread waiter([&] {
        auto lock = FileLock::acquire(lock_path);
        acquired = lock != nullptr;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    check("waiter blocks while held", !acquired);
    first.reset();
    waiter.join();
    check("waiter gets the lock once released", acquired);

    // A released waiter leaves nothing behind for the next holder.
    check("lock is free after the waiter exits", FileLock::try_acquire(lock_path) != nullptr);

    std::error_code ec;
    fs::remove_all(dir, ec);
    return agent_test::finish();
}

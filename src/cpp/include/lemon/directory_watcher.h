#pragma once

#include <functional>
#include <memory>
#include <string>

namespace lemon {

/// Abstract file-system-directory watcher.
///
/// Concrete implementations differ by platform:
///   - Linux  : inotify + epoll  (low latency, ~0-5ms)
///   - macOS  : kqueue           (~50ms latency)
///   - others : polling fallback (~200ms interval)
class DirectoryWatcher {
public:
    DirectoryWatcher(const std::string& dir_path);
    ~DirectoryWatcher();

    // Start the background watcher thread.
    // If @p dir_path does not exist yet, the watcher polls until it appears
    // (at most 30 seconds) and then begins watching.
    void start();

    // Signal the watcher to stop and wait for the thread to exit.
    void stop();

    // Set the callback invoked when the directory contents change.
    // Must be called *before* start(). The callback is executed on the
    // watcher thread (inside the debounce window); it must not throw.
    void set_callback(std::function<void()> callback);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lemon

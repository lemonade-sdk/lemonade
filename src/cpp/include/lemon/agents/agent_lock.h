#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace lemon::agents {

// An exclusive OS file lock (flock / LockFileEx), released when the object is
// destroyed or the process dies. It belongs to the open file, so threads of
// the holder never contend on it.
class FileLock {
public:
    explicit FileLock(std::intptr_t handle) : handle_(handle) {}
    ~FileLock();
    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

    // nullptr when another process holds it or the file can't be opened.
    static std::unique_ptr<FileLock> try_acquire(const std::filesystem::path& path);
    // Retries try_acquire until `wait` passes, for locks others hold only briefly.
    static std::unique_ptr<FileLock> try_acquire_for(const std::filesystem::path& path,
                                                     std::chrono::milliseconds wait);
    // Waits for the lock; nullptr only when the file can't be opened.
    static std::unique_ptr<FileLock> acquire(const std::filesystem::path& path);

    // Whether some process holds the lock right now.
    static bool is_held(const std::filesystem::path& path);

private:
    std::intptr_t handle_;
};

} // namespace lemon::agents

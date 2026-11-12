#include "lemon/agents/agent_lock.h"

#include <cerrno>
#include <cstdint>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace lemon::agents {
namespace {

std::unique_ptr<FileLock> lock_file(const fs::path& path, bool wait) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
#ifdef _WIN32
    HANDLE h = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    OVERLAPPED overlapped = {};
    const DWORD flags = LOCKFILE_EXCLUSIVE_LOCK | (wait ? 0 : LOCKFILE_FAIL_IMMEDIATELY);
    if (!LockFileEx(h, flags, 0, 1, 0, &overlapped)) {
        CloseHandle(h);
        return nullptr;
    }
    return std::make_unique<FileLock>(reinterpret_cast<std::intptr_t>(h));
#else
    const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        return nullptr;
    }
    int rc;
    do {
        rc = flock(fd, LOCK_EX | (wait ? 0 : LOCK_NB));
    } while (rc != 0 && errno == EINTR);
    if (rc != 0) {
        close(fd);
        return nullptr;
    }
    return std::make_unique<FileLock>(fd);
#endif
}

} // namespace

FileLock::~FileLock() {
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
    close(static_cast<int>(handle_));
#endif
}

std::unique_ptr<FileLock> FileLock::try_acquire(const fs::path& path) {
    return lock_file(path, false);
}

std::unique_ptr<FileLock> FileLock::try_acquire_for(const fs::path& path,
                                                    std::chrono::milliseconds wait) {
    const auto deadline = std::chrono::steady_clock::now() + wait;
    while (true) {
        if (auto lock = try_acquire(path)) {
            return lock;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return nullptr;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

std::unique_ptr<FileLock> FileLock::acquire(const fs::path& path) {
    return lock_file(path, true);
}

bool FileLock::is_held(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return false;
    }
    return try_acquire(path) == nullptr;
}

} // namespace lemon::agents

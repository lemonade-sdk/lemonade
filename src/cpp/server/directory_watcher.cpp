#include <lemon/directory_watcher.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <memory>
#include <functional>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <filesystem>

namespace fs = std::filesystem;

#ifdef __linux__
    #define HAS_EPOLL 1
    #include <sys/inotify.h>
    #include <sys/epoll.h>
    #include <sys/eventfd.h>
    #include <sys/stat.h>
    #include <unistd.h>
    #include <errno.h>
#elif defined(__APPLE__)
    #include <CoreServices/CoreServices.h>
    #include <condition_variable>
    #include <dispatch/dispatch.h>
    #include <mutex>
    #include <sys/stat.h>
#endif

#ifdef _WIN32
    #define pipe(fds) _pipe(fds, 4096, _O_BINARY)
    #include <sys/stat.h>
    #ifndef S_ISDIR
        #define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
    #endif
#else
    #include <fcntl.h>
#endif

namespace lemon {

#if defined(__linux__)
class DirectoryWatcher::Impl {
public:
    explicit Impl(const std::string& dir_path)
        : dir_path_(dir_path)
        , stop_flag_(false)
        , inotify_fd_(-1)
        , wd_(-1)
        , event_fd_(-1)
        , epoll_fd_(-1)
        , has_watch_(false)
    {}

    ~Impl() { stop(); }

    // event_fd_ is created here, before the thread exists, and closed only after
    // it is joined. run_loop() never reassigns it, so stop() always has a valid
    // fd to signal and neither thread writes a descriptor the other reads.
    void start() {
        event_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        thread_ = std::thread([this]() { run_loop(); });
    }

    void stop() {
        stop_flag_.store(true);
        if (event_fd_ >= 0) {
            uint64_t one = 1;
            ssize_t ret;
            do { ret = write(event_fd_, &one, sizeof(one)); }
            while (ret < 0 && errno == EINTR);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        // Sole owner again: the thread is gone and cannot race this close.
        if (event_fd_ >= 0) { ::close(event_fd_); event_fd_ = -1; }
    }

    void set_callback(std::function<void()> cb) { callback_ = std::move(cb); }

    void run_loop() {
        struct stat st;
        bool dir_exists = (stat(dir_path_.c_str(), &st) == 0 && S_ISDIR(st.st_mode));

        if (!dir_exists) {
            for (int attempt = 0; attempt < 60 && !stop_flag_.load(); ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                dir_exists = (stat(dir_path_.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
                if (dir_exists) break;
            }
            if (!dir_exists) {
                return;
            }
        }

        inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (inotify_fd_ < 0) {
            inotify_fd_ = -1;
            return;
        }

        unsigned int mask = IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF |
                            IN_MOVED_FROM | IN_MOVED_TO | IN_ISDIR |
                            IN_CLOSE_WRITE;
        wd_ = inotify_add_watch(inotify_fd_, dir_path_.c_str(), mask);
        if (wd_ < 0) {
            ::close(inotify_fd_);
            inotify_fd_ = -1;
            return;
        }

        if (event_fd_ < 0) {
            inotify_rm_watch(inotify_fd_, wd_);
            wd_ = -1;
            ::close(inotify_fd_);
            inotify_fd_ = -1;
            return;
        }

        epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ < 0) {
            inotify_rm_watch(inotify_fd_, wd_);
            wd_ = -1;
            ::close(inotify_fd_);
            inotify_fd_ = -1;
            return;
        }

        // Register both fds with epoll
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.fd = event_fd_;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, event_fd_, &ev);
        ev.events = EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP;
        ev.data.fd = inotify_fd_;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, inotify_fd_, &ev);

        has_watch_ = true;
        watch_paths_[wd_] = dir_path_;
        watch_subdirectories(dir_path_);

        constexpr int epoll_timeout_ms = 100;

        while (!stop_flag_.load()) {
            struct epoll_event events[4];
            int n = epoll_wait(epoll_fd_, events, 4, epoll_timeout_ms);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }

            // Check for stop signal on eventfd
            for (int i = 0; i < n; ++i) {
                if (events[i].data.fd == event_fd_) {
                    uint64_t dummy;
                    ssize_t ret;
                    do { ret = read(event_fd_, &dummy, sizeof(dummy)); }
                    while (ret < 0 && errno == EINTR);
                    stop_flag_.store(true);
                    break;
                }
            }
            if (stop_flag_.load()) break;

            // Drain all pending inotify events
            bool got_event = false;
            alignas(struct inotify_event) char buf[4096];
            ssize_t len;
            while ((len = read(inotify_fd_, buf, sizeof(buf))) > 0) {
                for (char* p = buf; p < buf + len;) {
                    const auto* event = reinterpret_cast<const struct inotify_event*>(p);
                    p += sizeof(struct inotify_event) + event->len;
                    if (event->mask & IN_IGNORED) {
                        watch_paths_.erase(event->wd);
                        continue;
                    }
                    auto parent = watch_paths_.find(event->wd);
                    if ((event->mask & IN_ISDIR) && (event->mask & (IN_CREATE | IN_MOVED_TO)) &&
                        event->len > 0 && parent != watch_paths_.end()) {
                        const fs::path created = parent->second / event->name;
                        add_nested_watch(created);
                        watch_subdirectories(created);
                    }
                    if (!is_file_being_written(*event, parent)) got_event = true;
                }
            }

            if (got_event) {
                std::this_thread::sleep_for(std::chrono::milliseconds(epoll_timeout_ms));
                if (stop_flag_.load()) break;
                if (callback_) callback_();
            }
        }

        if (epoll_fd_ >= 0) { ::close(epoll_fd_); epoll_fd_ = -1; }
        if (wd_ >= 0)       { inotify_rm_watch(inotify_fd_, wd_); wd_ = -1; }
        if (inotify_fd_ >= 0) { ::close(inotify_fd_); inotify_fd_ = -1; }
        watch_paths_.clear();
        has_watch_ = false;
    }

    // inotify watches one directory, not its subtree, and models sit in
    // nested folders whose contents (a new variant, a cache's refs/main) also
    // change what is listed. IN_MODIFY is left off so a file being downloaded
    // fires once when it closes, not on every write.
    void add_nested_watch(const fs::path& dir) {
        constexpr unsigned int nested_mask = IN_CREATE | IN_DELETE | IN_MOVED_FROM |
                                             IN_MOVED_TO | IN_CLOSE_WRITE | IN_ONLYDIR;
        int wd = inotify_add_watch(inotify_fd_, dir.c_str(), nested_mask);
        if (wd >= 0) watch_paths_[wd] = dir;
    }

    // A new regular file reports IN_CLOSE_WRITE once it is written, so its
    // IN_CREATE would only announce a file that may still be incomplete.
    // Symlinks (a cache snapshot's links into blobs/) and hard links never
    // close, so their IN_CREATE still counts.
    bool is_file_being_written(const struct inotify_event& event,
                               std::map<int, fs::path>::const_iterator parent) const {
        if (!(event.mask & IN_CREATE) || (event.mask & IN_ISDIR) || event.len == 0 ||
            parent == watch_paths_.end()) {
            return false;
        }
        struct stat st;
        const fs::path created = parent->second / event.name;
        return lstat(created.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1;
    }

    void watch_subdirectories(const fs::path& dir) {
        std::error_code ec;
        for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (it->is_directory(ec) && !it->is_symlink(ec)) add_nested_watch(it->path());
        }
    }

    std::string dir_path_;
    std::atomic<bool> stop_flag_;
    std::function<void()> callback_;
    std::thread thread_;
    std::map<int, fs::path> watch_paths_;
    int inotify_fd_;
    int wd_;
    int event_fd_;
    int epoll_fd_;
    bool has_watch_;
};

#elif defined(__APPLE__)
class DirectoryWatcher::Impl {
public:
    explicit Impl(const std::string& dir_path)
        : dir_path_(dir_path)
        , stop_flag_(false)
        , changed_(false)
    {}

    ~Impl() { stop(); }

    void start() {
        thread_ = std::thread([this]() { run_loop(); });
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_flag_.store(true);
        }
        wake_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void set_callback(std::function<void()> cb) { callback_ = std::move(cb); }

private:
    // FSEvents reports changes anywhere below the watched path, so nested model
    // folders and a cache repo's refs/main are seen without a descriptor per
    // folder, which a large tree would exhaust under kqueue.
    static void on_events(ConstFSEventStreamRef, void* info, size_t, void*,
                          const FSEventStreamEventFlags*, const FSEventStreamEventId*) {
        auto* self = static_cast<Impl*>(info);
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            self->changed_ = true;
        }
        self->wake_.notify_all();
    }

    void run_loop() {
        struct stat st;
        bool dir_exists = (stat(dir_path_.c_str(), &st) == 0 && S_ISDIR(st.st_mode));

        if (!dir_exists) {
            for (int attempt = 0; attempt < 60 && !stop_flag_.load(); ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                dir_exists = (stat(dir_path_.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
                if (dir_exists) break;
            }
            if (!dir_exists) return;
        }

        CFStringRef path = CFStringCreateWithCString(nullptr, dir_path_.c_str(), kCFStringEncodingUTF8);
        if (!path) return;
        const void* path_values[] = { path };
        CFArrayRef paths = CFArrayCreate(nullptr, path_values, 1, &kCFTypeArrayCallBacks);
        CFRelease(path);
        if (!paths) return;

        FSEventStreamContext context = {0, this, nullptr, nullptr, nullptr};
        constexpr CFTimeInterval latency_seconds = 0.05;
        FSEventStreamRef stream = FSEventStreamCreate(
            nullptr, &Impl::on_events, &context, paths, kFSEventStreamEventIdSinceNow,
            latency_seconds, kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer);
        CFRelease(paths);
        if (!stream) return;

        dispatch_queue_t queue = dispatch_queue_create("lemon.directory_watcher", DISPATCH_QUEUE_SERIAL);
        FSEventStreamSetDispatchQueue(stream, queue);
        if (FSEventStreamStart(stream)) {
            while (true) {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait_for(lock, std::chrono::milliseconds(200),
                               [this]() { return stop_flag_.load() || changed_; });
                if (stop_flag_.load()) break;
                const bool changed = changed_;
                // FSEvents has no IN_CLOSE_WRITE and reports a file being
                // downloaded on every latency window, so hold the callback
                // until the tree has been quiet: one call per download.
                while (changed_) {
                    changed_ = false;
                    wake_.wait_for(lock, std::chrono::milliseconds(200),
                                   [this]() { return stop_flag_.load() || changed_; });
                }
                if (stop_flag_.load()) break;
                lock.unlock();

                if (stat(dir_path_.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) break;
                if (changed && callback_) callback_();
            }
            FSEventStreamStop(stream);
        }
        FSEventStreamInvalidate(stream);
        FSEventStreamRelease(stream);
        // on_events runs on the queue and reads this Impl, so let any queued
        // call finish before the watcher can be destroyed.
        dispatch_sync_f(queue, nullptr, [](void*) {});
        dispatch_release(queue);
    }

    std::string dir_path_;
    std::atomic<bool> stop_flag_;
    std::function<void()> callback_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool changed_;
};

#else
class DirectoryWatcher::Impl {
public:
    explicit Impl(const std::string& dir_path)
        : dir_path_(dir_path)
        , stop_flag_(false)
    {}

    ~Impl() { stop(); }

    void start() {
        thread_ = std::thread([this]() { run_loop(); });
    }

    void stop() {
        stop_flag_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void set_callback(std::function<void()> cb) { callback_ = std::move(cb); }

private:
    // Take a snapshot of directory contents (set of "name+size+mtime" tuples).
    // Size and a high-resolution mtime are both included so that a delete +
    // rewrite of the same file within one wall-clock second is still detected:
    // ::stat's st_mtime has only 1-second resolution, which on Windows makes
    // such rewrites invisible to a name-and-second-only diff.
    static std::set<std::tuple<std::string, long long, long long>> take_snapshot(const std::string& dir) {
        std::set<std::tuple<std::string, long long, long long>> snap;
        for (const auto& entry : fs::recursive_directory_iterator(dir,
                     fs::directory_options::skip_permission_denied | fs::directory_options::follow_directory_symlink)) {
            std::error_code ec;
            auto mtime = fs::last_write_time(entry.path(), ec).time_since_epoch().count();
            if (ec) continue;
            long long size = 0;
            if (entry.is_regular_file(ec)) {
                size = static_cast<long long>(entry.file_size(ec));
                if (ec) size = 0;
            }
            snap.emplace(entry.path().filename().string(), size, static_cast<long long>(mtime));
        }
        return snap;
    }

    void run_loop() {
        struct stat st;
        bool dir_exists = (stat(dir_path_.c_str(), &st) == 0 && S_ISDIR(st.st_mode));

        if (!dir_exists) {
            for (int attempt = 0; attempt < 60 && !stop_flag_.load(); ++attempt) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                dir_exists = (stat(dir_path_.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
                if (dir_exists) break;
            }
            if (!dir_exists) return;
        }

        std::set<std::tuple<std::string, long long, long long>> prev = take_snapshot(dir_path_);

        while (!stop_flag_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (stop_flag_.load()) break;

            struct stat st2;
            if (stat(dir_path_.c_str(), &st2) != 0 || !S_ISDIR(st2.st_mode)) {
                break;
            }

            auto curr = take_snapshot(dir_path_);
            if (curr != prev) {
                prev = std::move(curr);
                if (callback_) callback_();
            }
        }
    }

    std::string dir_path_;
    std::atomic<bool> stop_flag_;
    std::function<void()> callback_;
    std::thread thread_;
};
#endif

DirectoryWatcher::DirectoryWatcher(const std::string& dir_path)
    : impl_(std::make_unique<Impl>(dir_path))
{}

DirectoryWatcher::~DirectoryWatcher() = default;

void DirectoryWatcher::stop() {
    impl_->stop();
}

void DirectoryWatcher::set_callback(std::function<void()> callback) {
    impl_->set_callback(std::move(callback));
}

void DirectoryWatcher::start() {
    impl_->start();
}

} // namespace lemon

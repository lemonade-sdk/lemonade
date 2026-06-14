#include <lemon/directory_watcher.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <memory>
#include <functional>
#include <cstring>
#include <set>
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
    #include <sys/event.h>
    #include <sys/stat.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <errno.h>
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

    void start() {
        thread_ = std::thread([this]() { run_loop(); });
    }

    void stop() {
        bool expected = false;
        if (stop_flag_.compare_exchange_strong(expected, true)) {
#ifdef HAS_EVENTFD
            if (event_fd_ >= 0) {
                uint64_t one = 1;
                ssize_t ret;
                do { ret = write(event_fd_, &one, sizeof(one)); }
                while (ret < 0 && errno == EINTR);
            }
#endif
            if (epoll_fd_ >= 0) {
                ::close(epoll_fd_);
                epoll_fd_ = -1;
            }
            if (event_fd_ >= 0) {
                ::close(event_fd_);
                event_fd_ = -1;
            }
            if (wd_ >= 0) {
                inotify_rm_watch(inotify_fd_, wd_);
                wd_ = -1;
            }
        }
        if (thread_.joinable()) {
            thread_.join();
        }
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
            return;
        }

        unsigned int mask = IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF |
                            IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY | IN_ISDIR |
                            IN_CLOSE_WRITE;
        wd_ = inotify_add_watch(inotify_fd_, dir_path_.c_str(), mask);
        if (wd_ < 0) {
            ::close(inotify_fd_);
            return;
        }

        event_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (event_fd_ < 0) {
            ::close(inotify_fd_);
            return;
        }

        epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ < 0) {
            ::close(event_fd_);
            event_fd_ = -1;
            ::close(inotify_fd_);
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
            for (int retries = 0; retries < 5; ++retries) {
                constexpr size_t buf_size = 4096;
                char buf[buf_size];
                ssize_t n2 = read(inotify_fd_, buf, buf_size);
                if (n2 < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    break;
                }
                if (n2 > 0) {
                    got_event = true;
                    break;
                }
            }

            if (got_event) {
                std::this_thread::sleep_for(std::chrono::milliseconds(epoll_timeout_ms));
                if (stop_flag_.load()) break;
                if (callback_) callback_();
            }
        }

        if (epoll_fd_ >= 0) { ::close(epoll_fd_); epoll_fd_ = -1; }
        if (event_fd_ >= 0) { ::close(event_fd_); event_fd_ = -1; }
        if (wd_ >= 0)       { inotify_rm_watch(inotify_fd_, wd_); wd_ = -1; }
        if (inotify_fd_ >= 0) { ::close(inotify_fd_); inotify_fd_ = -1; }
        has_watch_ = false;
    }

    std::string dir_path_;
    std::atomic<bool> stop_flag_;
    std::function<void()> callback_;
    std::thread thread_;
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
        , stop_pipe_read_(-1)
        , stop_pipe_write_(-1)
        , dir_fd_(-1)
        , kq_(-1)
    {}

    ~Impl() { stop(); }

    void start() {
        thread_ = std::thread([this]() { run_loop(); });
    }

    void stop() {
        bool expected = false;
        if (stop_flag_.compare_exchange_strong(expected, true)) {
            if (stop_pipe_write_ >= 0) {
                char byte = '\0';
                ssize_t ret;
                do { ret = ::write(stop_pipe_write_, &byte, 1); }
                while (ret < 0 && errno == EINTR);
            }
            if (kq_ >= 0) {
                ::close(kq_);
                kq_ = -1;
            }
            if (stop_pipe_read_ >= 0) {
                ::close(stop_pipe_read_);
                stop_pipe_read_ = -1;
            }
            if (stop_pipe_write_ >= 0) {
                ::close(stop_pipe_write_);
                stop_pipe_write_ = -1;
            }
            if (dir_fd_ >= 0) {
                ::close(dir_fd_);
                dir_fd_ = -1;
            }
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void set_callback(std::function<void()> cb) { callback_ = std::move(cb); }

private:
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

        kq_ = kqueue();
        if (kq_ < 0) return;

        // Open directory file descriptor for EVFILT_VNODE
        dir_fd_ = open(dir_path_.c_str(), O_RDONLY | O_CLOEXEC);
        if (dir_fd_ < 0) {
            ::close(kq_);
            kq_ = -1;
            return;
        }

        // Use a pipe for signaling instead of eventfd (not available on macOS)
        int stop_pipe_fds[2];
        if (pipe(stop_pipe_fds) < 0) {
            ::close(dir_fd_);
            dir_fd_ = -1;
            ::close(kq_);
            kq_ = -1;
            return;
        }
        // Set non-blocking on both ends
        int flags;
        flags = fcntl(stop_pipe_fds[0], F_GETFL);
        fcntl(stop_pipe_fds[0], F_SETFL, flags | O_NONBLOCK);
        flags = fcntl(stop_pipe_fds[1], F_GETFL);
        fcntl(stop_pipe_fds[1], F_SETFL, flags | O_NONBLOCK);
        stop_pipe_read_ = stop_pipe_fds[0]; // read end for kqueue/drain
        stop_pipe_write_ = stop_pipe_fds[1]; // write end for signaling

        struct kevent ev_dir;
        EV_SET(&ev_dir, dir_fd_,
               EVFILT_VNODE, EV_ADD | EV_ENABLE,
#ifndef NOTE_TRUNCATE
               NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_EXTEND | NOTE_ATTRIB,
#else
               NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_EXTEND | NOTE_TRUNCATE | NOTE_ATTRIB,
#endif
               0, nullptr);

        struct kevent ev_stop;
        EV_SET(&ev_stop, static_cast<intptr_t>(stop_pipe_read_),
               EVFILT_READ, EV_ADD | EV_ENABLE,
               0, 0, nullptr);

        struct kevent change_events[2] = { ev_dir, ev_stop };
        if (kevent(kq_, change_events, 2, nullptr, 0, nullptr) < 0) {
            ::close(stop_pipe_fds[0]);
            ::close(stop_pipe_fds[1]);
            ::close(dir_fd_);
            stop_pipe_read_ = -1;
            stop_pipe_write_ = -1;
            dir_fd_ = -1;
            ::close(kq_);
            kq_ = -1;
            return;
        }

        struct kevent events[4];

        while (!stop_flag_.load()) {
            constexpr int timeout_ms = 200;
            struct timespec ts;
            ts.tv_sec = timeout_ms / 1000;
            ts.tv_nsec = (timeout_ms % 1000) * 1000000L;

            int n = kevent(kq_, nullptr, 0, events, 4, &ts);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n == 0) continue;

            bool stop_detected = false;
            for (int i = 0; i < n; ++i) {
                if (events[i].ident == static_cast<uintptr_t>(stop_pipe_read_) &&
                    events[i].filter == EVFILT_READ) {
                    // Drain the pipe
                    char dummy[64];
                    ssize_t ret;
                    do { ret = ::read(stop_pipe_read_, dummy, sizeof(dummy)); }
                    while (ret < 0 && errno == EINTR);
                    stop_detected = true;
                    break;
                }
                if (events[i].filter == EVFILT_VNODE &&
                    events[i].udata != nullptr &&
                    events[i].fflags & NOTE_RENAME) {
                    continue;
                }
            }

            if (stop_detected) break;
            if (stop_flag_.load()) break;

            if (stat(dir_path_.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
                break;
            }

            if (callback_) callback_();
        }

        if (stop_pipe_write_ >= 0) { ::close(stop_pipe_write_); stop_pipe_write_ = -1; }
        if (stop_pipe_read_ >= 0) { ::close(stop_pipe_read_); stop_pipe_read_ = -1; }
        if (dir_fd_ >= 0)       { ::close(dir_fd_);       dir_fd_ = -1; }
        if (kq_ >= 0)           { ::close(kq_);           kq_ = -1; }
    }

    std::string dir_path_;
    std::atomic<bool> stop_flag_;
    std::function<void()> callback_;
    std::thread thread_;
    int stop_pipe_read_;
    int stop_pipe_write_;
    int dir_fd_;
    int kq_;
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
    // Take a snapshot of directory contents (set of "name+mtime" pairs).
    static std::set<std::pair<std::string, long long>> take_snapshot(const std::string& dir) {
        std::set<std::pair<std::string, long long>> snap;
        for (const auto& entry : fs::recursive_directory_iterator(dir,
                     fs::directory_options::skip_permission_denied | fs::directory_options::follow_directory_symlink)) {
            struct stat st;
            if (::stat(entry.path().string().c_str(), &st) != 0) continue;
            snap.emplace(entry.path().filename().string(), st.st_mtime);
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

        std::set<std::pair<std::string, long long>> prev = take_snapshot(dir_path_);

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

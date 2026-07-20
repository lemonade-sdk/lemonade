#include <lemon/suspend_inhibitor.h>

#include <mutex>

#ifdef HAVE_SYSTEMD
#include <systemd/sd-bus.h>
#include <unistd.h>
#endif

#include <lemon/utils/aixlog.hpp>

namespace lemon {

#ifdef HAVE_SYSTEMD

namespace {

// Take a logind delay/block inhibitor lock via D-Bus. Returns a dup'd fd the
// caller owns, or -1. Logs an error at debug level on failure.
int take_logind_inhibitor() {
    sd_bus* bus = nullptr;
    sd_bus_message* reply = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;

    int r = sd_bus_open_system(&bus);
    if (r < 0 || !bus) {
        LOG(DEBUG, "Suspend") << "logind: cannot connect to system bus; will not retry" << std::endl;
        sd_bus_error_free(&error);
        return -1;
    }

    r = sd_bus_call_method(
        bus,
        "org.freedesktop.login1",
        "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager",
        "Inhibit",
        &error,
        &reply,
        "ssss",
        "sleep:idle",
        "lemonade",
        "Inference in progress",
        "block"
    );

    if (r < 0 || !reply) {
        LOG(DEBUG, "Suspend") << "logind Inhibit failed: "
                              << (error.message ? error.message : "unknown dbus error")
                              << "; will not retry" << std::endl;
        sd_bus_error_free(&error);
        sd_bus_unref(bus);
        return -1;
    }

    // The fd is owned by the message; dup it so it survives sd_bus_message_unref.
    int lock_fd = -1;
    r = sd_bus_message_read(reply, "h", &lock_fd);
    int dup_fd = (r >= 0 && lock_fd >= 0) ? dup(lock_fd) : -1;

    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    sd_bus_unref(bus);

    if (dup_fd < 0) {
        LOG(DEBUG, "Suspend") << "logind: failed to read inhibitor fd; will not retry" << std::endl;
    }
    return dup_fd;
}

class LinuxSuspendInhibitor : public SuspendInhibitor {
public:
    ~LinuxSuspendInhibitor() override = default;

protected:
    void on_first_acquire() override {
        if (acquire_failed_) {
            return;
        }
        lock_fd_ = take_logind_inhibitor();
        if (lock_fd_ < 0) {
            acquire_failed_ = true;
        }
    }

    void on_last_release() override {
        if (lock_fd_ >= 0) {
            close(lock_fd_);
            lock_fd_ = -1;
        }
    }

private:
    int lock_fd_ = -1;
    bool acquire_failed_ = false;
};

} // namespace

std::unique_ptr<SuspendInhibitor> create_suspend_inhibitor() {
    return std::make_unique<LinuxSuspendInhibitor>();
}

#else // HAVE_SYSTEMD

namespace {
class NoopSuspendInhibitor : public SuspendInhibitor {
public:
    ~NoopSuspendInhibitor() override = default;
};
} // namespace

std::unique_ptr<SuspendInhibitor> create_suspend_inhibitor() {
    LOG(DEBUG, "Suspend") << "Built without systemd; suspend inhibition disabled" << std::endl;
    return std::make_unique<NoopSuspendInhibitor>();
}

#endif // HAVE_SYSTEMD

} // namespace lemon

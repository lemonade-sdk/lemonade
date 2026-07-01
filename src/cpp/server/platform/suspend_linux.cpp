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

// Takes a logind delay/block inhibitor lock. Returns a dup'd fd the caller owns, or -1.
// Writes a human-readable error into `out_error` on failure (empty on success).
int take_logind_inhibitor(const char*& out_error) {
    sd_bus* bus = nullptr;
    sd_bus_message* reply = nullptr;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    out_error = nullptr;

    int r = sd_bus_open_system(&bus);
    if (r < 0 || !bus) {
        out_error = "cannot connect to system bus";
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
        if (error.message) {
            out_error = error.message;
        } else {
            out_error = "unknown dbus error";
        }
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
        out_error = "failed to read inhibitor fd from logind";
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
        const char* err = nullptr;
        lock_fd_ = take_logind_inhibitor(err);
        if (lock_fd_ < 0) {
            acquire_failed_ = true;
            LOG(WARNING, "Suspend") << "logind suspend inhibition unavailable: " << err
                                    << "; will not retry" << std::endl;
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

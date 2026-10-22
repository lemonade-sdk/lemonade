#pragma once

#include "lemon/backends/backend_option_contract.h"

#include <atomic>
#include <mutex>
#include <stdexcept>

namespace lemon::backends {

class CatalogPublication {
public:
    enum class State { Empty, Building, Published, BuiltinsConsumed };
    State state() const { return state_.load(std::memory_order_acquire); }
    void begin() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state() != State::Empty) throw std::logic_error("Backend catalog was published, consumed, or is building");
        state_.store(State::Building, std::memory_order_release);
    }
    void rollback() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state() == State::Building) state_.store(State::Empty, std::memory_order_release);
    }
    void commit(std::shared_ptr<const BackendCatalogData> data, std::shared_ptr<const void> server) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state() != State::Building || !data || !server) throw std::logic_error("Incomplete catalog publication");
        data_ = std::move(data);
        server_ = std::move(server);
        state_.store(State::Published, std::memory_order_release);
    }
    const BackendCatalogData* data() {
        auto observed = state();
        if (observed == State::Empty) {
            std::lock_guard<std::mutex> lock(mutex_);
            observed = state();
            if (observed == State::Empty) {
                state_.store(State::BuiltinsConsumed, std::memory_order_release);
                observed = State::BuiltinsConsumed;
            }
        }
        if (observed == State::Building) throw std::logic_error("Backend catalog publication is in progress");
        return observed == State::Published ? data_.get() : nullptr;
    }
    const void* server() const {
        const auto observed = state();
        if (observed == State::Building) throw std::logic_error("Backend catalog publication is in progress");
        return observed == State::Published ? server_.get() : nullptr;
    }
    const void* consume_server() {
        data();
        return server();
    }
private:
    std::atomic<State> state_{State::Empty};
    std::mutex mutex_;
    std::shared_ptr<const BackendCatalogData> data_;
    std::shared_ptr<const void> server_;
};

inline CatalogPublication& catalog_publication() {
    static CatalogPublication publication;
    return publication;
}

class CatalogPublicationGuard {
public:
    explicit CatalogPublicationGuard(CatalogPublication& publication) : publication_(publication) { publication_.begin(); }
    ~CatalogPublicationGuard() { if (!committed_) publication_.rollback(); }
    CatalogPublicationGuard(const CatalogPublicationGuard&) = delete;
    CatalogPublicationGuard& operator=(const CatalogPublicationGuard&) = delete;
    void commit(std::shared_ptr<const BackendCatalogData> data, std::shared_ptr<const void> server) {
        publication_.commit(std::move(data), std::move(server));
        committed_ = true;
    }
private:
    CatalogPublication& publication_;
    bool committed_ = false;
};

}

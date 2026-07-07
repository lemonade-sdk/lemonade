#include "lemon/request_registry.h"
#include <iomanip>
#include <random>
#include <sstream>

namespace lemon {

namespace {

std::string generate_hex_string(size_t length) {
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, 15);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < length; ++i) {
        oss << dist(rng);
    }
    return oss.str();
}

} // namespace

std::string generate_request_id() {
    return generate_hex_string(32);
}

ActiveRequestGuard::ActiveRequestGuard(RequestRegistry* registry,
                                       const std::string& request_id,
                                       std::shared_ptr<std::atomic<bool>> cancel_flag)
    : registry_(registry), request_id_(request_id), cancel_flag_(std::move(cancel_flag)) {
}

ActiveRequestGuard::~ActiveRequestGuard() {
    if (!request_id_.empty() && registry_) {
        registry_->unregister_request(request_id_);
    }
}

ActiveRequestGuard::ActiveRequestGuard(ActiveRequestGuard&& other) noexcept
    : registry_(other.registry_),
      request_id_(std::move(other.request_id_)),
      cancel_flag_(std::move(other.cancel_flag_)) {
    other.registry_ = nullptr;
    other.request_id_.clear();
}

ActiveRequestGuard& ActiveRequestGuard::operator=(ActiveRequestGuard&& other) noexcept {
    if (this != &other) {
        if (!request_id_.empty() && registry_) {
            registry_->unregister_request(request_id_);
        }
        registry_ = other.registry_;
        request_id_ = std::move(other.request_id_);
        cancel_flag_ = std::move(other.cancel_flag_);
        other.registry_ = nullptr;
        other.request_id_.clear();
    }
    return *this;
}

ActiveRequestGuard RequestRegistry::register_request(const std::string& request_id,
                                                      const std::string& model_name,
                                                      const std::string& endpoint,
                                                      bool is_streaming,
                                                      void* server) {
    auto cancel_flag = std::make_shared<std::atomic<bool>>(false);

    ActiveRequest entry;
    entry.model_name = model_name;
    entry.endpoint = endpoint;
    entry.is_streaming = is_streaming;
    entry.start_time = std::chrono::steady_clock::now();
    entry.cancel_flag = cancel_flag;
    entry.server = server;

    std::string final_id = request_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Handle collision: append suffix if ID already exists
        if (requests_.count(final_id)) {
            int suffix = 2;
            while (requests_.count(final_id + "-" + std::to_string(suffix))) {
                ++suffix;
            }
            final_id = final_id + "-" + std::to_string(suffix);
        }
        entry.request_id = final_id;
        requests_[final_id] = std::move(entry);
    }

    return ActiveRequestGuard(this, final_id, cancel_flag);
}

void RequestRegistry::unregister_request(const std::string& request_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    requests_.erase(request_id);
}

bool RequestRegistry::cancel_request(const std::string& request_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = requests_.find(request_id);
    if (it == requests_.end()) {
        return false;
    }
    it->second.cancel_flag->store(true, std::memory_order_release);
    return true;
}

std::vector<ActiveRequest> RequestRegistry::list_active_requests() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ActiveRequest> result;
    result.reserve(requests_.size());
    for (const auto& [id, req] : requests_) {
        result.push_back(req);
    }
    return result;
}

int RequestRegistry::cancel_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    int count = 0;
    for (auto& [id, req] : requests_) {
        req.cancel_flag->store(true, std::memory_order_release);
        ++count;
    }
    return count;
}

} // namespace lemon

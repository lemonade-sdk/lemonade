#pragma once

// Two physical slots only. Each Hybrid call sends the full logical role history.
// llama.cpp KV cache is an optimization, never the authoritative history.
// Other clients retain llama.cpp's automatic slot scheduling.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace lemon::hybrid_slot_scope {
inline thread_local int private_slot = -1;

// Shared by ordinary and Hybrid model inference plus explicit KV erasure.
// Keyed per loaded model: other models are not blocked by this guard.
struct ModelGate {
    std::timed_mutex mutex;
    std::atomic<uint64_t> ordinary_generation{0};
};
inline std::mutex model_gates_mutex;
inline std::map<std::string, std::shared_ptr<ModelGate>> model_gates;
inline std::shared_ptr<ModelGate> gate_for(const std::string& model) {
    std::lock_guard<std::mutex> lock(model_gates_mutex);
    auto& gate = model_gates[model];
    if (!gate) gate = std::make_shared<ModelGate>();
    return gate;
}

class InferenceGuard {
public:
    explicit InferenceGuard(const std::string& model, bool enabled = true)
        : gate_(enabled ? gate_for(model) : nullptr) {
        if (gate_) {
            lock_ = std::unique_lock<std::timed_mutex>(gate_->mutex, std::defer_lock);
            if (!lock_.try_lock_for(std::chrono::seconds(15))) {
                throw std::runtime_error("Hybrid-enabled model busy over 15 seconds");
            }
        }
    }
    InferenceGuard(const InferenceGuard&) = delete;
    InferenceGuard& operator=(const InferenceGuard&) = delete;
private:
    std::shared_ptr<ModelGate> gate_;
    std::unique_lock<std::timed_mutex> lock_;
};

class Role {
public:
    explicit Role(int slot) noexcept : previous_(private_slot) {
        private_slot = (slot == 0 || slot == 1) ? slot : -1;
    }
    ~Role() { private_slot = previous_; }
    Role(const Role&) = delete;
    Role& operator=(const Role&) = delete;
private:
    int previous_;
};

inline int resolved_slot() noexcept {
    return private_slot == 0 || private_slot == 1 ? private_slot : -1;
}

// Called while holding the model gate, only for ordinary (non-hybrid) inference.
// An ordinary llama.cpp request is allowed to reuse either slot, but its KV
// is not evidence of Hybrid role pressure on the following request.
inline void note_ordinary_inference(const std::string& model, bool hybrid_slots) {
    if (hybrid_slots && resolved_slot() < 0) {
        gate_for(model)->ordinary_generation.fetch_add(1, std::memory_order_relaxed);
    }
}

inline uint64_t ordinary_generation(const std::string& model) {
    return gate_for(model)->ordinary_generation.load(std::memory_order_relaxed);
}
} // namespace lemon::hybrid_slot_scope

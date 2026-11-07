#pragma once

// Guarded heap memory for secret key material.
//
// Backed by sodium_malloc: the allocation sits between guard pages, carries a
// canary, is mlock'ed out of swap where the platform allows it, and is wiped
// on free. Use it for material that must not outlive its owner — FROST shares,
// epoch vote private keys, signing nonces.

#include <cstddef>
#include <cstdint>
#include <span>

namespace nexus::crypto {

class SecureBuffer {
public:
    SecureBuffer() = default;
    explicit SecureBuffer(std::size_t size);
    explicit SecureBuffer(std::span<const uint8_t> initial);

    SecureBuffer(const SecureBuffer&) = delete;
    SecureBuffer& operator=(const SecureBuffer&) = delete;

    SecureBuffer(SecureBuffer&& other) noexcept;
    SecureBuffer& operator=(SecureBuffer&& other) noexcept;

    ~SecureBuffer();

    [[nodiscard]] std::size_t size() const { return size_; }
    [[nodiscard]] bool empty() const { return size_ == 0; }

    [[nodiscard]] uint8_t* data() { return data_; }
    [[nodiscard]] const uint8_t* data() const { return data_; }

    [[nodiscard]] std::span<uint8_t> span() { return {data_, size_}; }
    [[nodiscard]] std::span<const uint8_t> span() const { return {data_, size_}; }

    /// Wipe and release now instead of at destruction.
    void clear();

private:
    uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace nexus::crypto

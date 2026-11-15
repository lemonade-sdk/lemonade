#include <LemonadeNexus/Crypto/SecureBuffer.hpp>

#include <sodium.h>

#include <algorithm>
#include <new>
#include <stdexcept>

namespace nexus::crypto {

namespace {

void ensure_sodium() {
    // Idempotent; SecureBuffer must work before any service lifecycle runs.
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialization failed");
    }
}

}  // namespace

SecureBuffer::SecureBuffer(std::size_t size) {
    if (size == 0) {
        return;
    }
    ensure_sodium();
    data_ = static_cast<uint8_t*>(sodium_malloc(size));
    if (data_ == nullptr) {
        throw std::bad_alloc();
    }
    size_ = size;
    sodium_memzero(data_, size_);
}

SecureBuffer::SecureBuffer(std::span<const uint8_t> initial) : SecureBuffer(initial.size()) {
    std::copy(initial.begin(), initial.end(), data_);
}

SecureBuffer::SecureBuffer(SecureBuffer&& other) noexcept
    : data_(other.data_), size_(other.size_) {
    other.data_ = nullptr;
    other.size_ = 0;
}

SecureBuffer& SecureBuffer::operator=(SecureBuffer&& other) noexcept {
    if (this != &other) {
        clear();
        data_ = other.data_;
        size_ = other.size_;
        other.data_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

SecureBuffer::~SecureBuffer() { clear(); }

void SecureBuffer::clear() {
    if (data_ != nullptr) {
        sodium_free(data_);  // sodium_free wipes the allocation before release
        data_ = nullptr;
    }
    size_ = 0;
}

}  // namespace nexus::crypto

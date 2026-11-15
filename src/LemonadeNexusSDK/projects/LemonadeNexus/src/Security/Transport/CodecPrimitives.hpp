#pragma once

// Internal byte writer and reader of the security wire codec. Shared by
// SecurityCodec.cpp and AuthorityChainCodec.cpp so both encode one layout.
// Not part of the public include tree.

#include <LemonadeNexus/Security/Policy/SecurityTypes.hpp>
#include <LemonadeNexus/Security/Transport/CodecError.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace nexus::security::codec_detail {

class Writer {
public:
    void u8(uint8_t value) { out_.push_back(value); }
    void u16(uint16_t value) { put_le(value, 2); }
    void u32(uint32_t value) { put_le(value, 4); }
    void u64(uint64_t value) { put_le(value, 8); }
    template <typename Tag>
    void u64(StrongId<Tag> value) { put_le(value.underlying(), 8); }
    void fixed(std::span<const uint8_t> bytes) { out_.insert(out_.end(), bytes.begin(), bytes.end()); }

    /// Length-prefixed field. Returns false when the field exceeds its bound,
    /// so a caller can never emit a message the decoder would reject.
    [[nodiscard]] bool bytes(std::span<const uint8_t> field, std::size_t max) {
        if (field.size() > max) {
            return false;
        }
        u32(static_cast<uint32_t>(field.size()));
        fixed(field);
        return true;
    }

    [[nodiscard]] std::vector<uint8_t> take() { return std::move(out_); }

private:
    void put_le(uint64_t value, int width) {
        for (int i = 0; i < width; ++i) {
            out_.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
        }
    }

    std::vector<uint8_t> out_;
};

class Reader {
public:
    explicit Reader(std::span<const uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] bool u8(uint8_t& value) { return read_le(value, 1); }
    [[nodiscard]] bool u16(uint16_t& value) { return read_le(value, 2); }
    [[nodiscard]] bool u32(uint32_t& value) { return read_le(value, 4); }
    [[nodiscard]] bool u64(uint64_t& value) { return read_le(value, 8); }
    template <typename Tag>
    [[nodiscard]] bool u64(StrongId<Tag>& value) {
        uint64_t raw = 0;
        if (!read_le(raw, 8)) return false;
        value = raw;
        return true;
    }

    template <std::size_t N>
    [[nodiscard]] bool fixed(std::array<uint8_t, N>& value) {
        if (remaining() < N) {
            return fail(CodecError::Truncated);
        }
        std::memcpy(value.data(), bytes_.data() + offset_, N);
        offset_ += N;
        return true;
    }

    /// The length is bounded before any allocation: an attacker-chosen
    /// length never reserves memory.
    [[nodiscard]] bool bytes(std::vector<uint8_t>& value, std::size_t max) {
        uint32_t length = 0;
        if (!u32(length)) {
            return false;
        }
        if (length > max) {
            return fail(CodecError::LengthTooLarge);
        }
        if (remaining() < length) {
            return fail(CodecError::Truncated);
        }
        value.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                     bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + length));
        offset_ += length;
        return true;
    }

    [[nodiscard]] std::size_t remaining() const { return bytes_.size() - offset_; }
    [[nodiscard]] bool done() const { return offset_ == bytes_.size(); }
    [[nodiscard]] std::optional<CodecError> error() const { return error_; }
    [[nodiscard]] bool fail(CodecError error) {
        if (!error_.has_value()) {
            error_ = error;
        }
        return false;
    }

private:
    template <typename T>
    bool read_le(T& value, std::size_t width) {
        if (remaining() < width) {
            return fail(CodecError::Truncated);
        }
        uint64_t raw = 0;
        for (std::size_t i = 0; i < width; ++i) {
            raw |= static_cast<uint64_t>(bytes_[offset_ + i]) << (i * 8);
        }
        value = static_cast<T>(raw);
        offset_ += width;
        return true;
    }

    std::span<const uint8_t> bytes_;
    std::size_t offset_ = 0;
    std::optional<CodecError> error_;
};

inline void put_node(Writer& w, const NodeId& node) { w.fixed(node.bytes); }

[[nodiscard]] inline bool get_node(Reader& r, NodeId& node) { return r.fixed(node.bytes); }

}  // namespace nexus::security::codec_detail

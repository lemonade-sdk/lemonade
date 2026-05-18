#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace lemon::utils::digest {

namespace detail {

inline uint32_t rotl32(uint32_t value, uint32_t bits) {
    return (value << bits) | (value >> (32U - bits));
}

inline uint32_t rotr32(uint32_t value, uint32_t bits) {
    return (value >> bits) | (value << (32U - bits));
}

inline std::string bytes_to_hex(const std::vector<unsigned char>& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (unsigned char byte : bytes) {
        oss << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return oss.str();
}

class Sha1 {
public:
    void update(const void* data, size_t len) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        total_bits_ += static_cast<uint64_t>(len) * 8U;
        while (len > 0) {
            const size_t take = std::min(len, block_.size() - block_used_);
            std::copy(bytes, bytes + take, block_.begin() + static_cast<std::ptrdiff_t>(block_used_));
            block_used_ += take;
            bytes += take;
            len -= take;
            if (block_used_ == block_.size()) {
                compress(block_.data());
                block_used_ = 0;
            }
        }
    }

    std::vector<unsigned char> final() {
        block_[block_used_++] = 0x80;
        if (block_used_ > 56) {
            std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_used_), block_.end(), 0);
            compress(block_.data());
            block_used_ = 0;
        }
        std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_used_), block_.begin() + 56, 0);
        for (int i = 7; i >= 0; --i) {
            block_[56 + (7 - i)] = static_cast<unsigned char>((total_bits_ >> (i * 8)) & 0xffU);
        }
        compress(block_.data());

        std::vector<unsigned char> out(20);
        const std::array<uint32_t, 5> state = {h0_, h1_, h2_, h3_, h4_};
        for (size_t i = 0; i < state.size(); ++i) {
            out[i * 4 + 0] = static_cast<unsigned char>((state[i] >> 24) & 0xffU);
            out[i * 4 + 1] = static_cast<unsigned char>((state[i] >> 16) & 0xffU);
            out[i * 4 + 2] = static_cast<unsigned char>((state[i] >> 8) & 0xffU);
            out[i * 4 + 3] = static_cast<unsigned char>(state[i] & 0xffU);
        }
        return out;
    }

private:
    void compress(const unsigned char* block) {
        uint32_t w[80] = {};
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h0_, b = h1_, c = h2_, d = h3_, e = h4_;
        for (int i = 0; i < 80; ++i) {
            uint32_t f = 0;
            uint32_t k = 0;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5a827999U;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1U;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdcU;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6U;
            }
            const uint32_t temp = rotl32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl32(b, 30);
            b = a;
            a = temp;
        }
        h0_ += a;
        h1_ += b;
        h2_ += c;
        h3_ += d;
        h4_ += e;
    }

    uint32_t h0_ = 0x67452301U;
    uint32_t h1_ = 0xefcdab89U;
    uint32_t h2_ = 0x98badcfeU;
    uint32_t h3_ = 0x10325476U;
    uint32_t h4_ = 0xc3d2e1f0U;
    uint64_t total_bits_ = 0;
    std::array<unsigned char, 64> block_{};
    size_t block_used_ = 0;
};

class Sha256 {
public:
    void update(const void* data, size_t len) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        total_bits_ += static_cast<uint64_t>(len) * 8U;
        while (len > 0) {
            const size_t take = std::min(len, block_.size() - block_used_);
            std::copy(bytes, bytes + take, block_.begin() + static_cast<std::ptrdiff_t>(block_used_));
            block_used_ += take;
            bytes += take;
            len -= take;
            if (block_used_ == block_.size()) {
                compress(block_.data());
                block_used_ = 0;
            }
        }
    }

    std::vector<unsigned char> final() {
        block_[block_used_++] = 0x80;
        if (block_used_ > 56) {
            std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_used_), block_.end(), 0);
            compress(block_.data());
            block_used_ = 0;
        }
        std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_used_), block_.begin() + 56, 0);
        for (int i = 7; i >= 0; --i) {
            block_[56 + (7 - i)] = static_cast<unsigned char>((total_bits_ >> (i * 8)) & 0xffU);
        }
        compress(block_.data());

        std::vector<unsigned char> out(32);
        for (size_t i = 0; i < h_.size(); ++i) {
            out[i * 4 + 0] = static_cast<unsigned char>((h_[i] >> 24) & 0xffU);
            out[i * 4 + 1] = static_cast<unsigned char>((h_[i] >> 16) & 0xffU);
            out[i * 4 + 2] = static_cast<unsigned char>((h_[i] >> 8) & 0xffU);
            out[i * 4 + 3] = static_cast<unsigned char>(h_[i] & 0xffU);
        }
        return out;
    }

private:
    static uint32_t ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ ((~x) & z); }
    static uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
    static uint32_t bsig0(uint32_t x) { return rotr32(x, 2) ^ rotr32(x, 13) ^ rotr32(x, 22); }
    static uint32_t bsig1(uint32_t x) { return rotr32(x, 6) ^ rotr32(x, 11) ^ rotr32(x, 25); }
    static uint32_t ssig0(uint32_t x) { return rotr32(x, 7) ^ rotr32(x, 18) ^ (x >> 3); }
    static uint32_t ssig1(uint32_t x) { return rotr32(x, 17) ^ rotr32(x, 19) ^ (x >> 10); }

    void compress(const unsigned char* block) {
        static constexpr uint32_t k[64] = {
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
            0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
            0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
            0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
            0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
            0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
            0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
        };

        uint32_t w[64] = {};
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            w[i] = ssig1(w[i - 2]) + w[i - 7] + ssig0(w[i - 15]) + w[i - 16];
        }

        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
        uint32_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = h + bsig1(e) + ch(e, f, g) + k[i] + w[i];
            const uint32_t t2 = bsig0(a) + maj(a, b, c);
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
        h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
    }

    std::array<uint32_t, 8> h_ = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };
    uint64_t total_bits_ = 0;
    std::array<unsigned char, 64> block_{};
    size_t block_used_ = 0;
};

template <typename Hasher>
inline std::string hash_file(const std::filesystem::path& path,
                             const std::string& prefix = std::string()) {
    Hasher hasher;
    if (!prefix.empty()) {
        hasher.update(prefix.data(), prefix.size());
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("failed to open file for hash verification");
    }

    std::array<char, 1024 * 1024> buffer{};
    while (file.good()) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = file.gcount();
        if (count > 0) {
            hasher.update(buffer.data(), static_cast<size_t>(count));
        }
    }
    if (file.bad()) {
        throw std::runtime_error("failed while reading file for hash verification");
    }

    return bytes_to_hex(hasher.final());
}

} // namespace detail

inline std::string sha256_file(const std::filesystem::path& path) {
    return detail::hash_file<detail::Sha256>(path);
}

inline std::string sha1_file(const std::filesystem::path& path) {
    return detail::hash_file<detail::Sha1>(path);
}

inline std::string git_blob_sha1_file(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        throw std::runtime_error("failed to get file size for git-sha1 verification: " + ec.message());
    }
    const std::string prefix = "blob " + std::to_string(size) + std::string(1, '\0');
    return detail::hash_file<detail::Sha1>(path, prefix);
}

} // namespace lemon::utils::digest

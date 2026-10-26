#pragma once

// Core protocol scalar types shared across the security subsystem.
//
// NodeId is a distinct struct, not an array alias: consensus code passes node
// identities and digests side by side, and an accidental swap must not compile.

#include <LemonadeNexus/Crypto/CryptoTypes.hpp>

#include <array>
#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace nexus::security {

// A 64-bit protocol scalar with its own type. EpochId, View, Height, and the
// other IDs below were plain uint64_t aliases, so any one of them could be
// swapped into another's parameter position and compile. Each is now a
// distinct instantiation of StrongId; a swap is a compile error.
//
// The wrapper is a transparent value: construction, comparison, and arithmetic
// are all delegated to the stored uint64_t, and .underlying() recovers it for
// wire/JSON/string encoders. It deliberately defines no conversions (in or
// out), so the compiler enforces the type boundaries. Arithmetic is the usual
// unsigned 64-bit wraparound, exactly as it was for raw uint64_t.
template <typename Tag> struct StrongId {
    uint64_t value{};

    constexpr StrongId() = default;
    constexpr StrongId(uint64_t v) : value(v) {}

    [[nodiscard]] constexpr uint64_t underlying() const { return value; }

    constexpr auto operator<=>(const StrongId&) const = default;

    friend constexpr StrongId operator+(StrongId a, StrongId b) { return a.value + b.value; }
    friend constexpr StrongId operator-(StrongId a, StrongId b) { return a.value - b.value; }
    friend constexpr StrongId operator+(StrongId a, uint64_t b) { return a.value + b; }
    friend constexpr StrongId operator+(uint64_t a, StrongId b) { return a + b.value; }
    friend constexpr StrongId operator-(StrongId a, uint64_t b) { return a.value - b; }
    friend constexpr StrongId operator-(uint64_t a, StrongId b) { return a - b.value; }

    friend constexpr StrongId& operator+=(StrongId& a, StrongId b) {
        a.value += b.value;
        return a;
    }
    friend constexpr StrongId& operator+=(StrongId& a, uint64_t b) {
        a.value += b;
        return a;
    }
    friend constexpr StrongId& operator-=(StrongId& a, StrongId b) {
        a.value -= b.value;
        return a;
    }
    friend constexpr StrongId& operator-=(StrongId& a, uint64_t b) {
        a.value -= b;
        return a;
    }
    friend constexpr StrongId& operator++(StrongId& a) {
        ++a.value;
        return a;
    }
    friend constexpr StrongId operator++(StrongId& a, int) {
        const StrongId old = a;
        ++a.value;
        return old;
    }
    friend constexpr StrongId& operator--(StrongId& a) {
        --a.value;
        return a;
    }
    friend constexpr StrongId operator--(StrongId& a, int) {
        const StrongId old = a;
        --a.value;
        return old;
    }

    [[nodiscard]] friend constexpr bool operator==(const StrongId& a, const StrongId& b) {
        return a.value == b.value;
    }
    [[nodiscard]] friend constexpr bool operator!=(const StrongId& a, const StrongId& b) {
        return a.value != b.value;
    }
    [[nodiscard]] friend constexpr bool operator==(const StrongId& a, uint64_t b) {
        return a.value == b;
    }
    [[nodiscard]] friend constexpr bool operator!=(const StrongId& a, uint64_t b) {
        return a.value != b;
    }
    [[nodiscard]] friend constexpr bool operator==(uint64_t a, const StrongId& b) {
        return a == b.value;
    }
    [[nodiscard]] friend constexpr bool operator!=(uint64_t a, const StrongId& b) {
        return a != b.value;
    }
    [[nodiscard]] friend constexpr bool operator<(const StrongId& a, const StrongId& b) {
        return a.value < b.value;
    }
    [[nodiscard]] friend constexpr bool operator>(const StrongId& a, const StrongId& b) {
        return a.value > b.value;
    }
    [[nodiscard]] friend constexpr bool operator<=(const StrongId& a, const StrongId& b) {
        return a.value <= b.value;
    }
    [[nodiscard]] friend constexpr bool operator>=(const StrongId& a, const StrongId& b) {
        return a.value >= b.value;
    }
};

/// Raw 64-bit value of a StrongId (any of the protocol IDs below). Mirrors
/// .underlying() for call sites that prefer a free function.
template <typename Id>
[[nodiscard]] constexpr auto underlying(const Id& id) -> decltype(id.underlying()) {
    return id.underlying();
}

struct EpochIdTag {};
struct ViewTag {};
struct HeightTag {};
struct KeyGenerationTag {};
struct IncarnationIdTag {};
struct OperationIdTag {};
struct SigningSessionIdTag {};

using EpochId = StrongId<EpochIdTag>;
using View = StrongId<ViewTag>;
using Height = StrongId<HeightTag>;
using KeyGeneration = StrongId<KeyGenerationTag>;
using IncarnationId = StrongId<IncarnationIdTag>;
using OperationId = StrongId<OperationIdTag>;
using SigningSessionId = StrongId<SigningSessionIdTag>;

}  // namespace nexus::security

/// std::hash for the protocol IDs, so they stay usable in unordered
/// containers. Hashes the raw 64-bit value: stable across runs for a fixed
/// value, equal for equal values, independent of the tag.
template <typename Tag>
struct std::hash<nexus::security::StrongId<Tag>> {
    [[nodiscard]] constexpr std::size_t operator()(nexus::security::StrongId<Tag> id) const
        noexcept {
        return std::hash<uint64_t>{}(id.underlying());
    }
};

namespace nexus::security {

using SecurityRulesetVersion = uint16_t;
using ConsensusRulesetVersion = uint16_t;

inline constexpr std::size_t kDigestSize = 32;
using Digest = std::array<uint8_t, kDigestSize>;

// The network identity derives from the genesis identity and the compiled
// rulesets (see Genesis/BootstrapCertificate.hpp). Genesis fixes it once and
// every later object binds to it.
using NetworkId = Digest;

inline constexpr std::size_t kNonceSize = 32;
using Nonce = std::array<uint8_t, kNonceSize>;

// A node's identity is its raw Ed25519 identity public key.
inline constexpr std::size_t kNodeIdSize = crypto::kEd25519PublicKeySize;

struct NodeId {
    std::array<uint8_t, kNodeIdSize> bytes{};

    auto operator<=>(const NodeId&) const = default;

    [[nodiscard]] std::span<const uint8_t> span() const { return bytes; }
};

}  // namespace nexus::security

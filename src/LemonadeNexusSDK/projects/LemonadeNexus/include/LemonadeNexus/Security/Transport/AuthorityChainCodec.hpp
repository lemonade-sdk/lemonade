#pragma once

// Standalone wire form of one authority chain page: the same field layout the
// AuthorityChainPage security message body uses, without the message
// envelope. The Tier 1 membership endpoint serves it, and the client SDK
// decodes it. The decoder applies the same bounds as the security codec. It
// verifies nothing; AuthorityChain.hpp does.

#include <LemonadeNexus/Security/Epoch/AuthorityChainPage.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace nexus::security {

/// Encodes `page`. Returns an empty vector when the page exceeds a wire bound
/// (more than kMaxActiveTier1 founders or members, or more than
/// kMaxHandoffChainLinks links).
[[nodiscard]] std::vector<uint8_t> encode_authority_chain_page(const AuthorityChainPage& page);

/// Decodes one complete page. nullopt when the bytes are malformed, exceed a
/// bound, are larger than kMaxSecurityMessageBytes, or have trailing bytes.
[[nodiscard]] std::optional<AuthorityChainPage> decode_authority_chain_page(
    std::span<const uint8_t> bytes);

}  // namespace nexus::security

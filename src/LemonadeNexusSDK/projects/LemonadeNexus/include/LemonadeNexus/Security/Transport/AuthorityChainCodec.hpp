#pragma once

// Standalone wire form of one authority chain page: the same field layout the
// AuthorityChainPage security message body uses, without the message
// envelope. The Tier 1 membership endpoint serves it, and the client SDK
// decodes it. By default the decoder applies the security codec's bounds; the
// HTTP route may serve larger pages to a client that asks. It verifies
// nothing; AuthorityChain.hpp does.

#include <LemonadeNexus/Security/Epoch/AuthorityChainPage.hpp>
#include <LemonadeNexus/Security/Policy/SecurityConstants.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace nexus::security {

/// Encodes `page`. Returns an empty vector when the page exceeds a wire bound
/// (more than kMaxActiveTier1 founders or members, or more than `max_links`
/// links). The security wire and a client that does not ask for larger pages
/// use kMaxHandoffChainLinks; GET /api/tier1/authority may use up to
/// kMaxHttpChainLinks.
[[nodiscard]] std::vector<uint8_t> encode_authority_chain_page(
    const AuthorityChainPage& page, std::size_t max_links = constants::kMaxHandoffChainLinks);

/// Decodes one complete page. nullopt when the bytes are malformed, claim more
/// than `max_links` links or kMaxActiveTier1 founders, are larger than
/// `max_bytes`, or have trailing bytes. The defaults are the security wire's
/// bounds; a client that asked for larger pages passes kMaxHttpChainLinks and
/// kMaxHttpChainPageBytes.
[[nodiscard]] std::optional<AuthorityChainPage> decode_authority_chain_page(
    std::span<const uint8_t> bytes, std::size_t max_links = constants::kMaxHandoffChainLinks,
    std::size_t max_bytes = constants::kMaxSecurityMessageBytes);

}  // namespace nexus::security

#pragma once

#include <LemonadeNexusSDK/Error.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace lnsdk {

/// A server that first-connect discovery found: the name its TLS certificate
/// carries, the port probed, and the address the region name resolved to.
struct DiscoveredServer {
    std::string host;     ///< certificate common name, lower case, below the network domain
    uint16_t    port{0};
    std::string address;  ///< dotted IPv4 address the region name resolved to
};

/// Bootstrap discovery, the same steps as the client app's DNS discovery:
/// resolve `<region>.seip.<domain>`, read each address's leaf certificate
/// common name over TLS, and keep the names below `domain`. The apex, a
/// wildcard, and a name outside the domain are dropped.
///
/// The TLS handshake does not verify the certificate, so the names are
/// candidates only. Connect to them with the verified-TLS transport, and let
/// discover_genesis_key() require agreement of several of them, before
/// trusting anything they say.
///
/// Blocks on DNS and on one TLS handshake per address (at most 16 addresses,
/// 5 s each). Errors: http_status 0 when `domain` or `region` is not a
/// hostname label set, the region name does not resolve, or no address
/// presented a usable name.
Result<std::vector<DiscoveredServer>> discover_servers(const std::string& domain,
                                                       const std::string& region,
                                                       uint16_t port = 9100);

/// discover_servers() for addresses already known (seed lists and tests):
/// probes each address of `addresses` in order and keeps the names below
/// `domain`. Duplicate names are reported once, at their first address.
Result<std::vector<DiscoveredServer>> discover_servers_at(const std::vector<std::string>& addresses,
                                                          const std::string& domain,
                                                          uint16_t port);

/// True when `name` is a hostname strictly below `domain`: not the apex, not
/// a wildcard, only hostname characters. Both are compared in lower case
/// without a trailing dot.
bool discovered_name_in_domain(const std::string& name, const std::string& domain);

}  // namespace lnsdk

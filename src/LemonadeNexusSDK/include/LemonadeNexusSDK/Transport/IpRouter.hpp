#pragma once

/// Userspace cryptokey routing table: maps a destination IPv4 address to the
/// peer whose allowed-IPs cover it, to "local" (one of our own virtual
/// addresses), or to a drop verdict. Replaces the kernel's allowed-ips
/// routing now that no TUN device exists.
///
/// Structure mirrors the real-world shape of the mesh: almost every route is
/// a /32 (client tunnel IPs, backbone IPs) so those live in a hash map;
/// the few wider prefixes (per-customer /30 private subnets) live in a small
/// vector scanned longest-prefix-first.
///
/// Pure logic, no I/O. Not internally synchronized — the owner serializes
/// mutation (UserspaceDataplane mutates only under its table lock).

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nexus::boringtun {

/// An IPv4 prefix in host byte order.
struct Cidr {
    uint32_t network{0};
    uint32_t mask{0};
    uint8_t  prefix_len{0};

    [[nodiscard]] bool contains(uint32_t ip) const { return (ip & mask) == network; }

    /// Parse "10.64.0.0/10" (or a bare address, treated as /32).
    /// Pure string/arithmetic parsing — no inet_pton, so it is
    /// platform-independent and trivially unit-testable.
    static std::optional<Cidr> parse(const std::string& text) {
        std::string ip_part = text;
        int prefix = 32;
        if (auto slash = text.find('/'); slash != std::string::npos) {
            ip_part = text.substr(0, slash);
            try {
                prefix = std::stoi(text.substr(slash + 1));
            } catch (...) {
                return std::nullopt;
            }
        }
        if (prefix < 0 || prefix > 32) return std::nullopt;

        uint32_t addr = 0;
        size_t pos = 0;
        for (int i = 0; i < 4; ++i) {
            size_t dot = ip_part.find('.', pos);
            bool last = (i == 3);
            // Exactly four dot-separated octets: no missing and no trailing parts.
            if (last != (dot == std::string::npos)) return std::nullopt;
            std::string oct = ip_part.substr(pos, last ? std::string::npos : dot - pos);
            if (oct.empty() || oct.size() > 3 ||
                !std::all_of(oct.begin(), oct.end(), [](char c) { return c >= '0' && c <= '9'; }))
                return std::nullopt;
            int v = std::stoi(oct);
            if (v > 255) return std::nullopt;
            addr = (addr << 8) | static_cast<uint32_t>(v);
            pos = dot + 1;
        }

        Cidr c;
        c.prefix_len = static_cast<uint8_t>(prefix);
        c.mask       = prefix == 0 ? 0u : ~((1ull << (32 - prefix)) - 1) & 0xFFFFFFFFu;
        c.network    = addr & c.mask;
        return c;
    }

    /// Parse a comma-separated allowed-IPs list; invalid entries are skipped.
    static std::vector<Cidr> parse_list(const std::string& allowed_ips) {
        std::vector<Cidr> out;
        size_t pos = 0;
        while (pos <= allowed_ips.size()) {
            size_t comma = allowed_ips.find(',', pos);
            std::string token = allowed_ips.substr(
                pos, comma == std::string::npos ? std::string::npos : comma - pos);
            auto start = token.find_first_not_of(" \t");
            if (start != std::string::npos) {
                auto end = token.find_last_not_of(" \t");
                if (auto c = parse(token.substr(start, end - start + 1))) out.push_back(*c);
            }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        return out;
    }
};

template <typename PeerPtr>
class IpRouter {
public:
    enum class Verdict { Local, Peer, Drop };

    struct Result {
        Verdict verdict{Verdict::Drop};
        PeerPtr peer{};
    };

    void add_local_ip(uint32_t ip) { local_ips_.insert(ip); }
    void remove_local_ip(uint32_t ip) { local_ips_.erase(ip); }

    /// Install routes for a peer's allowed IPs.
    void add_routes(const std::vector<Cidr>& cidrs, PeerPtr peer) {
        for (const auto& c : cidrs) {
            if (c.prefix_len == 32) {
                host_routes_[c.network] = peer;
            } else {
                // Replace any existing identical prefix, then keep the list
                // sorted by descending prefix length so a linear scan is LPM.
                std::erase_if(cidr_routes_, [&](const auto& r) {
                    return r.first.network == c.network && r.first.prefix_len == c.prefix_len;
                });
                cidr_routes_.emplace_back(c, peer);
                std::sort(cidr_routes_.begin(), cidr_routes_.end(),
                          [](const auto& a, const auto& b) {
                              return a.first.prefix_len > b.first.prefix_len;
                          });
            }
        }
    }

    /// Remove every route pointing at this peer.
    void remove_routes_for(const PeerPtr& peer) {
        std::erase_if(host_routes_, [&](const auto& kv) { return kv.second == peer; });
        std::erase_if(cidr_routes_, [&](const auto& r) { return r.second == peer; });
    }

    [[nodiscard]] Result lookup(uint32_t dst_ip) const {
        if (local_ips_.count(dst_ip)) return {Verdict::Local, {}};
        if (auto it = host_routes_.find(dst_ip); it != host_routes_.end())
            return {Verdict::Peer, it->second};
        for (const auto& [cidr, peer] : cidr_routes_)
            if (cidr.contains(dst_ip)) return {Verdict::Peer, peer};
        return {Verdict::Drop, {}};
    }

    [[nodiscard]] size_t route_count() const {
        return host_routes_.size() + cidr_routes_.size();
    }

private:
    std::unordered_set<uint32_t>                local_ips_;
    std::unordered_map<uint32_t, PeerPtr>       host_routes_;
    std::vector<std::pair<Cidr, PeerPtr>>       cidr_routes_;  // sorted: longest prefix first
};

} // namespace nexus::boringtun

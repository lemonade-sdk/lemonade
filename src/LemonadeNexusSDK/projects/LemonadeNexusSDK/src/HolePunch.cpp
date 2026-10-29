#include <LemonadeNexusSDK/HolePunch.hpp>

#include <LemonadeNexusSDK/BoringtunMesh.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <optional>
#include <set>

namespace lnsdk {

namespace {

/// Parse a decimal number of 1 to `max_digits` digits that is at most `max`.
std::optional<uint32_t> parse_number(const std::string& text, std::size_t max_digits,
                                     uint32_t max) {
    if (text.empty() || text.size() > max_digits) return std::nullopt;
    uint32_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + static_cast<uint32_t>(c - '0');
    }
    if (value > max) return std::nullopt;
    return value;
}

/// A dotted IPv4 address in host byte order.
std::optional<uint32_t> parse_ipv4(const std::string& text) {
    uint32_t address = 0;
    std::size_t start = 0;
    for (int i = 0; i < 4; ++i) {
        const auto dot = text.find('.', start);
        const bool last = i == 3;
        if (last != (dot == std::string::npos)) return std::nullopt;
        const auto part = text.substr(start, last ? std::string::npos : dot - start);
        const auto octet = parse_number(part, 3, 255);
        if (!octet) return std::nullopt;
        address = (address << 8) | *octet;
        start = dot + 1;
    }
    return address;
}

AddressScope address_scope(uint32_t ip) {
    const auto in = [ip](uint32_t network, int prefix) {
        const uint32_t mask = prefix == 0 ? 0 : ~uint32_t{0} << (32 - prefix);
        return (ip & mask) == network;
    };
    if (in(0x00000000, 8) || in(0xA9FE0000, 16) || in(0xE0000000, 4) || in(0xF0000000, 4)) {
        return AddressScope::Invalid;
    }
    if (in(0x7F000000, 8)) return AddressScope::Loopback;
    if (in(0x0A000000, 8) || in(0xAC100000, 12) || in(0xC0A80000, 16) || in(0x64400000, 10)) {
        return AddressScope::Private;
    }
    return AddressScope::Public;
}

int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int64_t unix_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string short_key(const std::string& key) {
    return key.substr(0, std::min<std::size_t>(8, key.size()));
}

}  // namespace

AddressScope endpoint_scope(const std::string& endpoint) {
    const auto colon = endpoint.find(':');
    if (colon == std::string::npos || endpoint.find(':', colon + 1) != std::string::npos) {
        return AddressScope::Invalid;
    }
    const auto port = parse_number(endpoint.substr(colon + 1), 5, 65535);
    const auto ip = parse_ipv4(endpoint.substr(0, colon));
    if (!port || *port == 0 || !ip) return AddressScope::Invalid;
    return address_scope(*ip);
}

bool reflexive_publishable(const std::string& reflexive, const std::string& stun_server_ip) {
    const auto server = endpoint_scope(stun_server_ip + ":1");
    switch (endpoint_scope(reflexive)) {
        case AddressScope::Public:
            return true;
        case AddressScope::Private:
            return server == AddressScope::Private || server == AddressScope::Loopback;
        case AddressScope::Loopback:
            return server == AddressScope::Loopback;
        case AddressScope::Invalid:
            break;
    }
    return false;
}

bool peer_endpoint_usable(const std::string& endpoint, AddressScope server_scope) {
    switch (endpoint_scope(endpoint)) {
        case AddressScope::Public:
        case AddressScope::Private:
            return true;
        case AddressScope::Loopback:
            return server_scope == AddressScope::Loopback;
        case AddressScope::Invalid:
            break;
    }
    return false;
}

HolePuncher::HolePuncher(BoringtunMesh& mesh) : HolePuncher(mesh, Timing{}) {}

HolePuncher::HolePuncher(BoringtunMesh& mesh, Timing timing) : mesh_(mesh), timing_(timing) {}

HolePuncher::Mode HolePuncher::punch_mode(bool has_endpoint, bool relayed, int64_t last_handshake,
                                          int64_t now_unix, bool prefer_direct,
                                          const Timing& timing) {
    if (!has_endpoint) return Mode::Idle;
    if (relayed) return prefer_direct ? Mode::Upgrade : Mode::Idle;
    if (last_handshake > 0 && now_unix - last_handshake < timing.handshake_fresh_sec) {
        return Mode::Idle;
    }
    return Mode::Direct;
}

int64_t HolePuncher::pair_slot_offset(const std::string& key_a, const std::string& key_b,
                                      int64_t interval_ms) {
    if (interval_ms <= 0) return 0;
    const auto& low = std::min(key_a, key_b);
    const auto& high = std::max(key_a, key_b);
    uint64_t hash = 0xcbf29ce484222325ULL;
    const auto mix = [&hash](unsigned char c) {
        hash ^= c;
        hash *= 0x100000001b3ULL;
    };
    for (const char c : low) mix(static_cast<unsigned char>(c));
    mix(0);
    for (const char c : high) mix(static_cast<unsigned char>(c));
    return static_cast<int64_t>(hash % static_cast<uint64_t>(interval_ms));
}

int64_t HolePuncher::next_slot_delay(int64_t now_unix_ms, int64_t interval_ms, int64_t offset_ms) {
    if (interval_ms <= 0) return 1;
    const int64_t phase = ((now_unix_ms + offset_ms) % interval_ms + interval_ms) % interval_ms;
    return interval_ms - phase;
}

HolePuncher::Clock::time_point HolePuncher::next_slot(Clock::time_point now,
                                                      const std::string& own,
                                                      const std::string& peer, Mode mode) const {
    const int64_t interval =
        (mode == Mode::Upgrade ? timing_.upgrade_interval : timing_.slot_interval).count();
    const int64_t delay =
        next_slot_delay(unix_now_ms(), interval, pair_slot_offset(own, peer, interval));
    return now + std::chrono::milliseconds(delay);
}

void HolePuncher::reset() { peers_.clear(); }

HolePuncher::Clock::time_point HolePuncher::maintain(
    const std::vector<MeshPeer>& peers, const std::map<std::string, std::string>& relayed,
    bool prefer_direct) {
    const auto now = Clock::now();
    const int64_t now_unix = unix_now();
    auto next_due = now + kMaxPassGap;
    const auto own = mesh_.public_key();
    if (own.empty()) {
        reset();
        return next_due;
    }

    std::map<std::string, int64_t> handshakes;
    for (const auto& p : mesh_.mesh_status().peers) handshakes[p.mesh_pubkey] = p.last_handshake;

    std::set<std::string> listed;
    std::size_t probes = 0;
    for (const auto& peer : peers) {
        const auto& pubkey = peer.mesh_pubkey;
        if (pubkey.empty() || pubkey == own) continue;
        listed.insert(pubkey);
        const auto found = handshakes.find(pubkey);
        const int64_t handshake = found == handshakes.end() ? 0 : found->second;
        const Mode mode = punch_mode(!peer.endpoint.empty(), relayed.contains(pubkey), handshake,
                                     now_unix, prefer_direct, timing_);
        if (mode == Mode::Idle) {
            peers_.erase(pubkey);
            continue;
        }

        auto& state = peers_[pubkey];
        if (state.target != peer.endpoint || state.mode != mode) {
            state.target = peer.endpoint;
            state.mode = mode;
            if (mode == Mode::Direct) {
                // A new endpoint: probe at once, in a burst.
                state.burst_left = timing_.burst_count;
                state.next = now;
            } else {
                // A relayed peer just failed its direct path: wait for a slot.
                state.burst_left = timing_.slot_burst;
                state.next = next_slot(now, own, pubkey, mode);
            }
        }

        if (now >= state.next) {
            if (probes >= kMaxProbesPerPass) {
                next_due = std::min(next_due, now + kMinPassGap);
                continue;
            }
            ++probes;
            if (mesh_.probe_peer(pubkey, state.target)) {
                ++probes_sent_;
                spdlog::debug("[HolePuncher] probe to peer {} at {} ({})", short_key(pubkey),
                              state.target, mode == Mode::Direct ? "direct" : "upgrade");
            }
            if (state.burst_left > 0) --state.burst_left;
            if (state.burst_left > 0) {
                state.next = now + timing_.burst_interval;
            } else {
                state.burst_left = std::max<uint32_t>(timing_.slot_burst, 1);
                state.next = next_slot(now, own, pubkey, mode);
            }
        }
        next_due = std::min(next_due, state.next);
    }

    std::erase_if(peers_, [&listed](const auto& entry) { return !listed.contains(entry.first); });
    return std::max(next_due, now + kMinPassGap);
}

}  // namespace lnsdk

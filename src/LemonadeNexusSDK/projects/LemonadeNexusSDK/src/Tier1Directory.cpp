#include <LemonadeNexusSDK/Tier1Directory.hpp>

#include <LemonadeNexus/Crypto/CryptoTypes.hpp>
#include <LemonadeNexus/Security/Epoch/Tier1Membership.hpp>
#include <LemonadeNexus/Security/Genesis/BootstrapCertificate.hpp>
#include <LemonadeNexus/Security/Policy/SecurityConstants.hpp>
#include <LemonadeNexus/Security/Transport/AuthorityChainCodec.hpp>

#include <nlohmann/json.hpp>
#include <sodium.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <mutex>

namespace lnsdk {

namespace {

namespace sec = nexus::security;
using json = nlohmann::json;

/// Pages one refresh walks before it gives up. Each page holds at most
/// kMaxHandoffChainLinks links, so this bounds the epochs per refresh.
constexpr int kMaxChainPages = 4096;
/// Candidate servers one refresh probes.
constexpr std::size_t kMaxProbes = 32;
/// Chain walks a refresh starts again after a proof names a later epoch.
constexpr int kMaxRewalks = 2;
/// Hosts one Genesis key discovery asks.
constexpr std::size_t kMaxGenesisDiscoveryHosts = 16;

int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string node_b64(const sec::NodeId& node) { return nexus::crypto::to_base64(node.bytes); }

/// Base64 from a server or a caller: malformed input is nullopt, never an
/// exception (nexus::crypto::from_base64 throws, and the SDK must not let an
/// exception reach the C ABI).
std::optional<std::vector<uint8_t>> decode_b64(std::string_view text) {
    try {
        return nexus::crypto::from_base64(text);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/// A string field, or empty when absent or of another type.
std::string string_field(const json& doc, const char* field) {
    const auto it = doc.find(field);
    return it != doc.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

/// A base64 field of exactly N bytes, or nullopt.
template <std::size_t N>
std::optional<std::array<uint8_t, N>> fixed_b64(const json& doc, const char* field) {
    if (!doc.contains(field) || !doc[field].is_string()) return std::nullopt;
    const auto bytes = decode_b64(doc[field].get<std::string>());
    if (!bytes || bytes->size() != N) return std::nullopt;
    std::array<uint8_t, N> out{};
    std::memcpy(out.data(), bytes->data(), N);
    return out;
}

enum class Walk { Done, Advanced, Unreachable, Refused };

/// A DNS name in compare form: lowercase, without a trailing dot.
std::string dns_name(std::string name) {
    name = lower(std::move(name));
    if (!name.empty() && name.back() == '.') name.pop_back();
    return name;
}

/// True when `host` is `domain` or a name below it.
bool inside_domain(const std::string& host, const std::string& domain) {
    return host == domain ||
           (host.size() > domain.size() + 1 && host.ends_with(domain) &&
            host[host.size() - domain.size() - 1] == '.');
}

/// What one host reported on GET /api/onboard/info.
struct GenesisClaim {
    nexus::crypto::Ed25519PublicKey key{};
    std::string network_id;  // lowercase hex
    std::string domain;      // compare form
};

}  // namespace

struct Tier1Directory::State {
    Tier1DiscoveryGet get;

    mutable std::mutex mutex;  // guards every field below except the walker
    std::optional<nexus::crypto::Ed25519PublicKey> genesis;
    uint64_t min_epoch = 0;
    uint64_t generation = 0;  // bumps when the pinned key changes
    Tier1Membership membership;
    std::optional<sec::VerifiedEpochAuthority> authority;
    int64_t verified_at_ms = 0;
    bool stale = true;
    std::string preferred;  // "host:port" of the server that answered last

    std::mutex refresh_mutex;  // serializes refresh(); guards the walker
    std::unique_ptr<sec::AuthorityChainWalker> walker;
    uint64_t walker_generation = 0;

    /// Pins `key` and raises the epoch floor. A different key drops every
    /// verified result. Caller holds `mutex`.
    void pin_locked(const nexus::crypto::Ed25519PublicKey& key, uint64_t floor) {
        if (!genesis || *genesis != key) {
            genesis = key;
            ++generation;
            membership = {};
            authority.reset();
            stale = true;
            preferred.clear();
        }
        min_epoch = std::max(min_epoch, floor);
    }

    /// Walks pages from `server` until it has nothing newer. io: network.
    Walk walk_from(const ServerEndpoint& server, std::string& error) {
        bool advanced = false;
        for (int page_count = 0; page_count < kMaxChainPages; ++page_count) {
            const auto have = walker->epoch().underlying();
            const auto reply =
                get(server, "/api/tier1/authority?have_epoch=" + std::to_string(have));
            if (!reply) return advanced ? Walk::Advanced : Walk::Unreachable;
            if (reply->status == 404) return advanced ? Walk::Advanced : Walk::Done;
            if (reply->status != 200) return advanced ? Walk::Advanced : Walk::Unreachable;
            const auto doc = json::parse(reply->body, nullptr, false);
            if (doc.is_discarded() || !doc.is_object() || !doc.contains("page") ||
                !doc["page"].is_string()) {
                error = "malformed authority chain page from " + server.host;
                return Walk::Refused;
            }
            const auto bytes = decode_b64(doc["page"].get<std::string>());
            const auto page = bytes ? sec::decode_authority_chain_page(*bytes)
                                    : std::optional<sec::AuthorityChainPage>{};
            if (!page) {
                error = "malformed authority chain page from " + server.host;
                return Walk::Refused;
            }
            switch (walker->apply(*page)) {
                case sec::ChainPageResult::Advanced:
                    advanced = true;
                    break;
                case sec::ChainPageResult::NoProgress:
                    return advanced ? Walk::Advanced : Walk::Done;
                case sec::ChainPageResult::Refused:
                    error = "the authority chain from " + server.host +
                            " does not verify against the pinned Genesis key";
                    return Walk::Refused;
            }
        }
        return advanced ? Walk::Advanced : Walk::Done;
    }
};

Tier1Directory::Tier1Directory(Tier1DiscoveryGet get) : state_(std::make_unique<State>()) {
    state_->get = std::move(get);
    if (sodium_init() < 0) {
        spdlog::error("[Tier1Directory] libsodium failed to initialize");
    }
}

Tier1Directory::~Tier1Directory() = default;

bool Tier1Directory::set_genesis_key(const std::string& genesis_pubkey_b64, uint64_t min_epoch) {
    const auto bytes = decode_b64(genesis_pubkey_b64);
    if (!bytes || bytes->size() != nexus::crypto::kEd25519PublicKeySize) return false;
    nexus::crypto::Ed25519PublicKey key{};
    std::memcpy(key.data(), bytes->data(), key.size());
    std::lock_guard lock(state_->mutex);
    state_->pin_locked(key, min_epoch);
    return true;
}

bool Tier1Directory::has_genesis_key() const {
    std::lock_guard lock(state_->mutex);
    return state_->genesis.has_value();
}

Result<GenesisKeyDiscovery> Tier1Directory::discover_genesis_key(
    const std::vector<ServerEndpoint>& hosts) {
    Result<GenesisKeyDiscovery> result;
    const auto fail = [&result](std::string why, int status) {
        result.error = std::move(why);
        result.http_status = status;
        return result;
    };
    constexpr const char* kPinned =
        "a Genesis key is already pinned; discovery never replaces it";
    // Server answers are hostile input. Nothing below should throw on them;
    // the catch is the backstop, so no exception leaves the SDK.
    try {
        std::lock_guard discovery_lock(state_->refresh_mutex);
        if (has_genesis_key()) return fail(kPinned, 409);

        // One question per distinct host name; ports do not make hosts distinct.
        std::vector<ServerEndpoint> targets;
        std::vector<std::string> seen;
        for (const auto& ep : hosts) {
            const auto name = dns_name(ep.host);
            if (name.empty() || std::find(seen.begin(), seen.end(), name) != seen.end()) {
                continue;
            }
            seen.push_back(name);
            targets.push_back(ep);
            if (targets.size() == kMaxGenesisDiscoveryHosts) break;
        }

        std::optional<GenesisClaim> agreed;
        std::string agreed_from;
        std::vector<std::string> agreeing;
        for (const auto& ep : targets) {
            const auto reply = state_->get(ep, "/api/onboard/info");
            if (!reply || reply->status != 200) continue;
            const auto doc = json::parse(reply->body, nullptr, false);
            if (doc.is_discarded() || !doc.is_object() || !doc.contains("genesis_pubkey")) {
                continue;  // no claim: a server without a configured key, or an older one
            }
            const auto key = fixed_b64<nexus::crypto::kEd25519PublicKeySize>(doc, "genesis_pubkey");
            if (!key) return fail("malformed Genesis key from " + ep.host, 403);
            const auto reported = lower(string_field(doc, "network_id"));
            const auto derived = nexus::crypto::to_hex(sec::derive_network_id(
                *key, sec::constants::kSecurityRulesetVersion,
                sec::constants::kConsensusRulesetVersion));
            if (reported != derived) {
                return fail("the network id from " + ep.host +
                                " does not derive from its Genesis key",
                            403);
            }
            GenesisClaim claim{*key, derived, dns_name(string_field(doc, "dns_base_domain"))};
            if (!agreed) {
                agreed = claim;
                agreed_from = ep.host;
            } else if (claim.key != agreed->key || claim.domain != agreed->domain) {
                return fail("servers disagree on the Genesis key or network domain: " +
                                agreed_from + " and " + ep.host,
                            403);
            }
            if (inside_domain(dns_name(ep.host), claim.domain)) agreeing.push_back(ep.host);
        }

        if (agreed && agreed->domain.empty()) {
            return fail("the servers report no network DNS domain, so their agreement "
                        "proves nothing; set the Genesis key explicitly",
                        403);
        }
        if (agreeing.size() < kGenesisDiscoveryMinHosts) {
            return fail("Genesis key discovery needs " +
                            std::to_string(kGenesisDiscoveryMinHosts) +
                            " agreeing servers inside the network domain, found " +
                            std::to_string(agreeing.size()) +
                            "; set the Genesis key explicitly",
                        0);
        }

        {
            std::lock_guard lock(state_->mutex);
            if (state_->genesis) return fail(kPinned, 409);
            state_->pin_locked(agreed->key, 0);
        }
        result.ok = true;
        result.value.genesis_pubkey = nexus::crypto::to_base64(agreed->key);
        result.value.network_id = agreed->network_id;
        result.value.dns_base_domain = agreed->domain;
        result.value.hosts = std::move(agreeing);
        return result;
    } catch (const std::exception& e) {
        return fail(std::string("Genesis key discovery failed: ") + e.what(), 403);
    }
}

Result<Tier1Membership> Tier1Directory::refresh(const std::vector<ServerEndpoint>& sources) {
    // Server answers are hostile input. Nothing below should throw on them;
    // this is the backstop, so no exception leaves the SDK.
    try {
        return refresh_verified(sources);
    } catch (const std::exception& e) {
        std::lock_guard lock(state_->mutex);
        state_->membership.servers.clear();
        state_->stale = true;
        Result<Tier1Membership> result;
        result.error = std::string("Tier 1 verification failed: ") + e.what();
        result.http_status = 403;
        return result;
    }
}

Result<Tier1Membership> Tier1Directory::refresh_verified(
    const std::vector<ServerEndpoint>& sources) {
    Result<Tier1Membership> result;
    std::lock_guard refresh_lock(state_->refresh_mutex);

    std::optional<nexus::crypto::Ed25519PublicKey> genesis;
    uint64_t min_epoch = 0;
    uint64_t generation = 0;
    {
        std::lock_guard lock(state_->mutex);
        genesis = state_->genesis;
        min_epoch = state_->min_epoch;
        generation = state_->generation;
    }
    // http_status 0: nothing reachable; 403: what the servers returned, or
    // the missing key, is refused.
    const auto fail = [&](std::string why, int status) {
        std::lock_guard lock(state_->mutex);
        if (state_->generation == generation) {
            state_->membership.servers.clear();
            state_->stale = true;
        }
        spdlog::warn("[Tier1Directory] {}", why);
        result.http_status = status;
        result.error = std::move(why);
        return result;
    };
    if (!genesis) {
        return fail("no pinned Genesis key: Tier 1 servers cannot be verified", 403);
    }
    if (!state_->walker || state_->walker_generation != generation) {
        state_->walker = std::make_unique<sec::AuthorityChainWalker>(*genesis);
        state_->walker_generation = generation;
    }

    // 1. The authority chain, from the pinned key.
    std::string chain_error;
    for (const auto& source : sources) {
        const auto walk = state_->walk_from(source, chain_error);
        if (walk == Walk::Done || walk == Walk::Advanced) break;
    }
    const auto& authority = state_->walker->authority();
    if (!authority) {
        return chain_error.empty()
                   ? fail("no server returned a verifiable authority chain", 0)
                   : fail(chain_error, 403);
    }
    if (state_->walker->epoch().underlying() < min_epoch) {
        return fail("the verified membership is stale: epoch " +
                    std::to_string(state_->walker->epoch().underlying()) +
                    " is older than the lowest accepted epoch " + std::to_string(min_epoch),
                    403);
    }

    // 2. Candidates: the sources, and the servers they list. A listed node
    // id is only a hint for skipping non-members; the proof decides.
    struct Candidate {
        ServerEndpoint endpoint;
        std::optional<sec::NodeId> listed_node;
    };
    std::vector<Candidate> candidates;
    const auto add_candidate = [&candidates](ServerEndpoint endpoint,
                                             std::optional<sec::NodeId> node) {
        if (endpoint.host.empty() || endpoint.port == 0) return;
        const auto key = lower(endpoint.host);
        for (const auto& c : candidates) {
            if (lower(c.endpoint.host) == key && c.endpoint.port == endpoint.port) return;
        }
        endpoint.use_tls = true;
        candidates.push_back(Candidate{std::move(endpoint), node});
    };
    for (const auto& source : sources) add_candidate(source, std::nullopt);
    for (const auto& source : sources) {
        const auto reply = state_->get(source, "/api/servers");
        if (!reply || reply->status != 200) continue;
        const auto doc = json::parse(reply->body, nullptr, false);
        if (doc.is_discarded() || !doc.is_array()) continue;
        for (const auto& entry : doc) {
            if (!entry.is_object()) continue;
            const auto host = string_field(entry, "fqdn");
            const auto port_it = entry.find("http_port");
            if (port_it == entry.end() || !port_it->is_number_unsigned() ||
                port_it->get<uint64_t>() > 0xFFFF) {
                continue;
            }
            const auto port = static_cast<uint16_t>(port_it->get<uint64_t>());
            const auto pubkey = string_field(entry, "pubkey");
            std::optional<sec::NodeId> listed;
            if (!pubkey.empty()) {
                const auto bytes = decode_b64(pubkey);
                sec::NodeId node{};
                if (!bytes || bytes->size() != node.bytes.size()) continue;
                std::memcpy(node.bytes.data(), bytes->data(), node.bytes.size());
                listed = node;
            }
            add_candidate(ServerEndpoint{host, port, true}, listed);
        }
        break;
    }

    // 3. Endpoint proofs over fresh nonces.
    std::vector<Tier1Server> verified;
    int rewalks = 0;
    std::size_t probes = 0;
    for (std::size_t i = 0; i < candidates.size() && probes < kMaxProbes; ++i) {
        const auto& candidate = candidates[i].endpoint;
        const auto& listed = candidates[i].listed_node;
        if (listed && std::find(authority->members.begin(), authority->members.end(),
                                *listed) == authority->members.end()) {
            continue;  // listed, but not a member of the verified epoch
        }
        ++probes;
        std::array<uint8_t, sec::kTier1EndpointNonceBytes> nonce{};
        randombytes_buf(nonce.data(), nonce.size());
        const auto reply = state_->get(
            candidate, "/api/tier1/proof?nonce=" + nexus::crypto::to_hex(nonce));
        if (!reply || reply->status != 200) continue;
        const auto doc = json::parse(reply->body, nullptr, false);
        if (doc.is_discarded() || !doc.is_object()) continue;
        const auto node = fixed_b64<32>(doc, "node_id");
        const auto signature = fixed_b64<64>(doc, "signature");
        if (!node || !signature || !doc.contains("epoch") || !doc["epoch"].is_number_unsigned() ||
            !doc.contains("host") || !doc["host"].is_string() || !doc.contains("port") ||
            !doc["port"].is_number_unsigned()) {
            continue;
        }
        const auto port = doc["port"].get<uint64_t>();
        const auto host = doc["host"].get<std::string>();
        if (port > 0xFFFF || host.size() > sec::kTier1EndpointMaxHostBytes) continue;
        sec::Tier1EndpointProof proof;
        proof.node.bytes = *node;
        proof.epoch = sec::EpochId{doc["epoch"].get<uint64_t>()};
        proof.host = host;
        proof.port = static_cast<uint16_t>(port);
        proof.signature = *signature;
        const auto check = sec::check_tier1_endpoint_proof(*state_->walker->authority(), proof,
                                                           candidate.host, candidate.port, nonce);
        if (check == sec::EndpointProofCheck::Valid) {
            verified.push_back(Tier1Server{node_b64(proof.node), candidate.host, candidate.port});
            continue;
        }
        if (check == sec::EndpointProofCheck::NewerEpoch && rewalks < kMaxRewalks) {
            // The verified membership is behind this member. Walk further
            // before trusting anyone, then probe every candidate again.
            ++rewalks;
            std::string ignored;
            if (state_->walk_from(candidate, ignored) == Walk::Advanced) {
                verified.clear();
                probes = 0;
                i = static_cast<std::size_t>(-1);
            }
            continue;
        }
        spdlog::debug("[Tier1Directory] {}:{} is not a Tier 1 endpoint: {}", candidate.host,
                      candidate.port, sec::endpoint_proof_check_name(check));
    }
    if (verified.empty()) {
        return fail("no verified Tier 1 server is reachable", 0);
    }

    Tier1Membership membership;
    membership.epoch = state_->walker->epoch().underlying();
    for (const auto& member : state_->walker->authority()->members) {
        membership.members.push_back(node_b64(member));
    }
    membership.servers = std::move(verified);
    {
        std::lock_guard lock(state_->mutex);
        if (state_->generation != generation) {
            result.error = "the pinned Genesis key changed during the refresh";
            return result;
        }
        state_->membership = membership;
        state_->authority = *state_->walker->authority();
        state_->verified_at_ms = steady_ms();
        state_->stale = false;
    }
    spdlog::info("[Tier1Directory] verified Tier 1 epoch {}: {} member(s), {} reachable server(s)",
                 membership.epoch, membership.members.size(), membership.servers.size());
    result.ok = true;
    result.value = std::move(membership);
    return result;
}

Tier1Membership Tier1Directory::membership() const {
    std::lock_guard lock(state_->mutex);
    return state_->membership;
}

bool Tier1Directory::needs_refresh(int64_t max_age_ms) const {
    std::lock_guard lock(state_->mutex);
    return state_->stale || state_->membership.servers.empty() ||
           steady_ms() - state_->verified_at_ms > max_age_ms;
}

bool Tier1Directory::is_verified_member(const std::string& node_id) const {
    std::lock_guard lock(state_->mutex);
    return std::any_of(state_->membership.servers.begin(), state_->membership.servers.end(),
                       [&node_id](const Tier1Server& s) { return s.node_id == node_id; });
}

void Tier1Directory::mark_stale() {
    std::lock_guard lock(state_->mutex);
    state_->stale = true;
}

std::optional<HttpReply> Tier1Directory::send(const Tier1Attempt& attempt,
                                              const std::string& pin_node,
                                              Tier1Server* answered, std::string& error) {
    std::vector<Tier1Server> servers;
    std::string preferred;
    {
        std::lock_guard lock(state_->mutex);
        servers = state_->membership.servers;
        preferred = state_->preferred;
    }
    if (servers.empty()) {
        error = "no verified Tier 1 server";
        return std::nullopt;
    }
    // Start with the server that answered last.
    const auto key_of = [](const Tier1Server& s) {
        return lower(s.host) + ":" + std::to_string(s.port);
    };
    const auto first = std::find_if(servers.begin(), servers.end(),
                                    [&](const Tier1Server& s) { return key_of(s) == preferred; });
    if (first != servers.end()) std::rotate(servers.begin(), first, servers.end());

    bool tried = false;
    std::string last;
    for (const auto& server : servers) {
        if (!pin_node.empty() && server.node_id != pin_node) continue;
        tried = true;
        auto reply = attempt(ServerEndpoint{server.host, server.port, true});
        if (!reply) {
            last = "no response from " + server.host;
            continue;
        }
        if (reply->status == 421) {
            // It holds no tree (any more): fail over, and verify again soon.
            mark_stale();
            last = server.host + " does not serve the tree";
            continue;
        }
        {
            std::lock_guard lock(state_->mutex);
            state_->preferred = key_of(server);
        }
        if (answered) *answered = server;
        return reply;
    }
    if (!tried) {
        error = "the session's Tier 1 server is not in the verified membership; "
                "authenticate again";
    } else {
        error = last.empty() ? "no verified Tier 1 server answered" : last;
    }
    return std::nullopt;
}

}  // namespace lnsdk

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
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <future>
#include <mutex>
#include <thread>

namespace lnsdk {

namespace {

namespace sec = nexus::security;
using json = nlohmann::json;

/// Pages one refresh walks before it gives up. Each page holds at most
/// kMaxHttpChainLinks links, so this bounds the epochs per refresh.
constexpr int kMaxChainPages = 4096;
/// Links a chain page request asks for. A server that predates max_links
/// ignores it and sends kMaxHandoffChainLinks.
constexpr std::size_t kChainPageLinks = nexus::security::constants::kMaxHttpChainLinks;
/// 429 answers one walk waits out before it treats the source as unreachable.
/// A server that predates the separate authority page limit allows a burst of
/// 20 requests; the waits let a walk from Genesis finish there too.
constexpr int kMaxRateLimitWaits = 30;
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

/// Characters of a transport reason kept in a note, so a failure message
/// that lists several servers stays readable.
constexpr std::size_t kMaxReasonChars = 64;

/// What a GET that brought no usable response is called in a failure
/// message: "HTTP <status>" for a response, else "unreachable" with the
/// transport's reason when it supplied one.
std::string reply_note(const std::optional<HttpReply>& reply) {
    if (reply && reply->status != 0) return "HTTP " + std::to_string(reply->status);
    std::string note = "unreachable";
    if (!reply || reply->error.empty()) return note;
    std::string why = reply->error.substr(0, kMaxReasonChars);
    for (auto& c : why) {
        if (std::iscntrl(static_cast<unsigned char>(c))) c = ' ';
    }
    if (reply->error.size() > kMaxReasonChars) why += "...";
    return note + " (" + why + ")";
}

/// `label` and the first 8 of `notes`, "; " apart; empty for no notes.
std::string notes_detail(const std::string& label, const std::vector<std::string>& notes) {
    std::string detail;
    for (std::size_t i = 0; i < notes.size() && i < 8; ++i) {
        detail += i == 0 ? label : "; ";
        detail += notes[i];
    }
    if (notes.size() > 8) detail += "; more";
    return detail;
}

}  // namespace

struct Tier1Directory::State {
    Tier1DiscoveryGet get;
    std::chrono::milliseconds rate_limit_wait{1000};

    mutable std::mutex mutex;  // guards every field below except the walker
    std::optional<nexus::crypto::Ed25519PublicKey> genesis;
    uint64_t min_epoch = 0;
    /// The chain instance this client knows under the pinned key: the Epoch 1
    /// anchor digest a caller passed with set_epoch_floor, or the one the last
    /// successful refresh verified. A verified chain with another Epoch 1 is
    /// a re-founded network, never a refresh of this one.
    std::optional<sec::Digest> known_chain;
    bool refounded = false;
    uint64_t generation = 0;  // bumps when the pinned key changes
    Tier1Membership membership;
    std::optional<sec::VerifiedEpochAuthority> authority;
    int64_t verified_at_ms = 0;
    bool stale = true;
    std::string preferred;  // "host:port" of the server that answered last

    std::mutex refresh_mutex;  // serializes refresh(); guards the walker
    std::unique_ptr<sec::AuthorityChainWalker> walker;
    uint64_t walker_generation = 0;

    // Progress of the running walk (walk_progress), read without a lock.
    std::atomic<bool> walk_active{false};
    std::atomic<uint64_t> walk_epoch{0};
    std::atomic<uint64_t> walk_target{0};
    std::atomic<uint64_t> walk_pages{0};
    std::atomic<uint64_t> walk_page_links{0};  // links in the last page applied

    /// Pins `key` and raises the epoch floor. A different key drops every
    /// verified result, the floor and the known chain with it: epochs of one
    /// Genesis say nothing about another. Caller holds `mutex`.
    void pin_locked(const nexus::crypto::Ed25519PublicKey& key, uint64_t floor) {
        if (!genesis || *genesis != key) {
            genesis = key;
            ++generation;
            membership = {};
            authority.reset();
            stale = true;
            preferred.clear();
            min_epoch = 0;
            known_chain.reset();
            refounded = false;
        }
        min_epoch = std::max(min_epoch, floor);
    }

    /// Walks pages from `server` until it has nothing newer. io: network.
    /// `behind` is set when the server reports a latest epoch below the one
    /// the walker holds, or reports none at all on "nothing newer": the
    /// server may serve another chain under the same key. `error` carries
    /// the refusal reason for Refused, and what the server answered for
    /// Unreachable (reply_note).
    Walk walk_from(const ServerEndpoint& server, std::string& error, bool& behind) {
        bool advanced = false;
        int waits = 0;
        for (int page_count = 0; page_count < kMaxChainPages; ++page_count) {
            const auto have = walker->epoch().underlying();
            const auto reply = get(server, "/api/tier1/authority?have_epoch=" +
                                               std::to_string(have) +
                                               "&max_links=" + std::to_string(kChainPageLinks));
            if (!reply || reply->status == 0) {
                error = reply_note(reply);
                return advanced ? Walk::Advanced : Walk::Unreachable;
            }
            // A rate limit is no verdict on the chain: wait for the server's
            // budget to refill and ask for the same page again.
            if (reply->status == 429 && waits < kMaxRateLimitWaits) {
                ++waits;
                --page_count;
                std::this_thread::sleep_for(rate_limit_wait);
                continue;
            }
            if (reply->status == 404) {
                const auto doc = json::parse(reply->body, nullptr, false);
                const bool hinted = !doc.is_discarded() && doc.is_object() &&
                                    doc.contains("latest_epoch") &&
                                    doc["latest_epoch"].is_number_unsigned();
                if (!hinted || doc["latest_epoch"].get<uint64_t>() < have) behind = true;
                return advanced ? Walk::Advanced : Walk::Done;
            }
            if (reply->status != 200) {
                error = reply_note(reply);
                return advanced ? Walk::Advanced : Walk::Unreachable;
            }
            const auto doc = json::parse(reply->body, nullptr, false);
            if (doc.is_discarded() || !doc.is_object() || !doc.contains("page") ||
                !doc["page"].is_string()) {
                error = "malformed authority chain page from " + server.host;
                return Walk::Refused;
            }
            const auto bytes = decode_b64(doc["page"].get<std::string>());
            const auto page =
                bytes ? sec::decode_authority_chain_page(*bytes, kChainPageLinks,
                                                         sec::constants::kMaxHttpChainPageBytes)
                      : std::optional<sec::AuthorityChainPage>{};
            if (!page) {
                error = "malformed authority chain page from " + server.host;
                return Walk::Refused;
            }
            if (doc.contains("latest_epoch") && doc["latest_epoch"].is_number_unsigned()) {
                // A display hint only; the walk verifies every page.
                const auto latest = doc["latest_epoch"].get<uint64_t>();
                if (latest > walk_target.load()) walk_target.store(latest);
                if (latest < have) behind = true;
            }
            switch (walker->apply(*page)) {
                case sec::ChainPageResult::Advanced:
                    advanced = true;
                    walk_pages.fetch_add(1);
                    walk_epoch.store(walker->epoch().underlying());
                    if (!page->links.empty()) walk_page_links.store(page->links.size());
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

    /// The Epoch 1 anchor of the chain `server` serves, when its base page
    /// verifies under the pinned key; nullopt when the server has no base,
    /// does not answer, or serves a base the key did not sign. io: network.
    std::optional<sec::Digest> other_chain_base(const ServerEndpoint& server) {
        const auto reply = get(server, "/api/tier1/authority?have_epoch=0&max_links=1");
        if (!reply || reply->status != 200) return std::nullopt;
        const auto doc = json::parse(reply->body, nullptr, false);
        if (doc.is_discarded() || !doc.is_object() || !doc.contains("page") ||
            !doc["page"].is_string()) {
            return std::nullopt;
        }
        const auto bytes = decode_b64(doc["page"].get<std::string>());
        if (!bytes) return std::nullopt;
        const auto page = sec::decode_authority_chain_page(*bytes, kChainPageLinks,
                                                           sec::constants::kMaxHttpChainPageBytes);
        if (!page || !page->has_base || !genesis) return std::nullopt;
        sec::AuthorityChainWalker fresh(*genesis);
        if (fresh.apply(*page) != sec::ChainPageResult::Advanced) return std::nullopt;
        return fresh.epoch_one_anchor();
    }
};

namespace {

/// The first 12 hex digits of a digest, for log and error text.
std::string short_hex(const sec::Digest& digest) {
    return nexus::crypto::to_hex(digest).substr(0, 12);
}

}  // namespace

Tier1Directory::Tier1Directory(Tier1DiscoveryGet get, std::chrono::milliseconds rate_limit_wait)
    : state_(std::make_unique<State>()) {
    state_->get = std::move(get);
    state_->rate_limit_wait = rate_limit_wait;
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

bool Tier1Directory::set_epoch_floor(const std::string& chain, uint64_t min_epoch) {
    sec::Digest anchor{};
    if (chain.size() != anchor.size() * 2) return false;
    const auto bytes = nexus::crypto::from_hex(chain);
    if (bytes.size() != anchor.size()) return false;
    std::memcpy(anchor.data(), bytes.data(), anchor.size());
    std::lock_guard lock(state_->mutex);
    state_->known_chain = anchor;
    state_->min_epoch = std::max(state_->min_epoch, min_epoch);
    return true;
}

bool Tier1Directory::network_refounded() const {
    std::lock_guard lock(state_->mutex);
    return state_->refounded;
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
        // What each host answered, so a "too few" failure shows who was
        // asked and why each one did not count.
        std::vector<std::string> asked;
        const auto note = [&asked](const ServerEndpoint& ep, const std::string& outcome) {
            asked.push_back(ep.host + ":" + std::to_string(ep.port) + " " + outcome);
        };
        for (const auto& ep : targets) {
            const auto reply = state_->get(ep, "/api/onboard/info");
            if (!reply || reply->status != 200) {
                note(ep, reply_note(reply));
                continue;
            }
            const auto doc = json::parse(reply->body, nullptr, false);
            if (doc.is_discarded() || !doc.is_object() || !doc.contains("genesis_pubkey")) {
                // A server without a configured key, or an older one.
                note(ep, "no claim");
                continue;
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
            if (inside_domain(dns_name(ep.host), claim.domain)) {
                agreeing.push_back(ep.host);
                note(ep, "agrees");
            } else {
                note(ep, "outside domain " + claim.domain);
            }
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
                            notes_detail("; asked " + std::to_string(targets.size()) +
                                             " host(s): ",
                                         asked) +
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
    std::optional<sec::Digest> known_chain;
    uint64_t known_epoch = 0;  // the epoch this client last verified or was given
    {
        std::lock_guard lock(state_->mutex);
        genesis = state_->genesis;
        min_epoch = state_->min_epoch;
        generation = state_->generation;
        known_chain = state_->known_chain;
        known_epoch = std::max(min_epoch, state_->membership.epoch);
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
    state_->walk_pages.store(0);
    state_->walk_page_links.store(0);
    state_->walk_epoch.store(state_->walker->epoch().underlying());
    state_->walk_target.store(min_epoch);
    state_->walk_active.store(true);
    struct WalkDone {
        std::atomic<bool>& active;
        ~WalkDone() { active.store(false); }
    } walk_done{state_->walk_active};

    // 1. The authority chain, from the pinned key.
    // A single source may hold only part of the chain: a non-Tier-1 replica
    // serves its gossiped store, which can be empty or behind the current
    // epoch. "Nothing more here" from one source is not a verdict - the next
    // source may hold the missing base or the next handoff. The sweep stops on
    // a chain that reaches the epoch floor, so a healthy first source still
    // costs one request; a source that refused the page or never answered is
    // never a verdict, even when an earlier walk already meets the floor.
    // Each source's outcome is recorded so the failure message shows who was
    // asked and what they offered.
    std::string chain_error;  // last refusal reason (a hostile or malformed page)
    std::vector<std::string> source_notes;
    std::optional<ServerEndpoint> settled_source;  // a source that may serve another chain
    const auto sweep = [&] {
        settled_source.reset();
        for (const auto& source : sources) {
            std::string source_error;
            bool behind = false;
            const auto walk = state_->walk_from(source, source_error, behind);
            const auto epoch = state_->walker->epoch().underlying();
            if (walk == Walk::Refused) chain_error = source_error;
            std::string state;
            if (walk == Walk::Refused) {
                state = "refused";
            } else if (walk == Walk::Unreachable) {
                state = source_error.empty() ? "unreachable" : source_error;
            } else if (walk == Walk::Advanced) {
                state = "advanced to epoch " + std::to_string(epoch);
            } else {
                state = "nothing new from epoch " + std::to_string(epoch);
                if (!settled_source && behind && state_->walker->authority()) {
                    settled_source = source;
                }
            }
            source_notes.push_back(source.host + ":" + std::to_string(source.port) + " " +
                                   state);
            const auto settled = walk == Walk::Done || walk == Walk::Advanced;
            if (settled && state_->walker->authority() && epoch >= min_epoch) break;
        }
    };
    sweep();
    // A walker that holds a chain hears "nothing new" from a server that
    // serves another chain under the same key, because that server has no
    // page after the epoch the walker names; a server that is simply up to
    // date says so with a latest epoch at or above it. One base page from a
    // source that reported less, or nothing, tells the chains apart: a base
    // that verifies under the pinned key but carries another Epoch 1 anchor
    // is a re-founded network, and the walk starts over on it so the outcome
    // names its epoch. A forged base does not verify and changes nothing.
    if (settled_source) {
        const auto other = state_->other_chain_base(*settled_source);
        if (other && *other != state_->walker->epoch_one_anchor()) {
            if (!known_chain) known_chain = state_->walker->epoch_one_anchor();
            known_epoch = std::max(known_epoch, state_->walker->epoch().underlying());
            state_->walker = std::make_unique<sec::AuthorityChainWalker>(*genesis);
            state_->walk_epoch.store(0);
            sweep();
        }
    }
    const auto tried_detail = [&] { return notes_detail("; tried: ", source_notes); };
    const auto& authority = state_->walker->authority();
    if (!authority) {
        return chain_error.empty()
                   ? fail("no server returned a verifiable authority chain" + tried_detail(), 0)
                   : fail(chain_error + tried_detail(), 403);
    }
    const sec::Digest chain = state_->walker->epoch_one_anchor();
    if (known_chain && *known_chain != chain) {
        // Another Epoch 1 under the pinned key. The floor belongs to the
        // chain this client knows and says nothing about this one, and this
        // one is never adopted on its own: the user leaves and joins again.
        {
            std::lock_guard lock(state_->mutex);
            if (state_->generation == generation) state_->refounded = true;
        }
        // The next refresh walks from the base again, so the known chain is
        // verified at once if the servers serve it again.
        const auto served_epoch = state_->walker->epoch().underlying();
        state_->walker.reset();
        return fail("the network was re-founded under the pinned Genesis key: this device "
                        "last verified epoch " + std::to_string(known_epoch) + " of chain " +
                        short_hex(*known_chain) + ", the servers now serve chain " +
                        short_hex(chain) + " at epoch " + std::to_string(served_epoch) +
                        "; leave the network and join it again" + tried_detail(),
                    409);
    }
    if (const auto epoch = state_->walker->epoch().underlying(); epoch < min_epoch) {
        return fail("the verified membership is stale: epoch " + std::to_string(epoch) +
                        " is older than the lowest accepted epoch " + std::to_string(min_epoch) +
                        tried_detail(),
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

    // 3. Endpoint proofs over fresh nonces, asked of every candidate at once:
    //    a round costs one request time, not one per member. The answers are
    //    judged in candidate order, so the verified list is the same list the
    //    one-by-one round gave.
    struct Probe {
        std::size_t candidate = 0;
        std::array<uint8_t, sec::kTier1EndpointNonceBytes> nonce{};
        std::optional<HttpReply> reply;
    };
    std::vector<Tier1Server> verified;
    // What each probed candidate answered, for the failure message.
    std::vector<std::string> probe_notes;
    int rewalks = 0;
    for (bool again = true; again;) {
        again = false;
        verified.clear();
        probe_notes.clear();
        std::vector<Probe> probes;
        for (std::size_t i = 0; i < candidates.size() && probes.size() < kMaxProbes; ++i) {
            const auto& listed = candidates[i].listed_node;
            if (listed && std::find(authority->members.begin(), authority->members.end(),
                                    *listed) == authority->members.end()) {
                continue;  // listed, but not a member of the verified epoch
            }
            Probe probe;
            probe.candidate = i;
            randombytes_buf(probe.nonce.data(), probe.nonce.size());
            probes.push_back(probe);
        }
        std::vector<std::future<std::optional<HttpReply>>> replies;
        replies.reserve(probes.size());
        for (const auto& probe : probes) {
            replies.push_back(std::async(
                std::launch::async,
                [this, endpoint = candidates[probe.candidate].endpoint,
                 path = "/api/tier1/proof?nonce=" + nexus::crypto::to_hex(probe.nonce)] {
                    return state_->get(endpoint, path);
                }));
        }
        for (std::size_t i = 0; i < probes.size(); ++i) probes[i].reply = replies[i].get();

        for (const auto& probe : probes) {
            const auto& candidate = candidates[probe.candidate].endpoint;
            const auto& reply = probe.reply;
            const auto note = [&](const std::string& outcome) {
                probe_notes.push_back(candidate.host + ":" + std::to_string(candidate.port) +
                                      " " + outcome);
            };
            if (!reply || reply->status != 200) {
                note(reply_note(reply));
                continue;
            }
            const auto doc = json::parse(reply->body, nullptr, false);
            if (doc.is_discarded() || !doc.is_object()) {
                note("malformed proof");
                continue;
            }
            const auto node = fixed_b64<32>(doc, "node_id");
            const auto signature = fixed_b64<64>(doc, "signature");
            if (!node || !signature || !doc.contains("epoch") ||
                !doc["epoch"].is_number_unsigned() || !doc.contains("host") ||
                !doc["host"].is_string() || !doc.contains("port") ||
                !doc["port"].is_number_unsigned()) {
                note("malformed proof");
                continue;
            }
            const auto port = doc["port"].get<uint64_t>();
            const auto host = doc["host"].get<std::string>();
            if (port > 0xFFFF || host.size() > sec::kTier1EndpointMaxHostBytes) {
                note("malformed proof");
                continue;
            }
            sec::Tier1EndpointProof proof;
            proof.node.bytes = *node;
            proof.epoch = sec::EpochId{doc["epoch"].get<uint64_t>()};
            proof.host = host;
            proof.port = static_cast<uint16_t>(port);
            proof.signature = *signature;
            const auto check = sec::check_tier1_endpoint_proof(
                *state_->walker->authority(), proof, candidate.host, candidate.port, probe.nonce);
            if (check == sec::EndpointProofCheck::Valid) {
                verified.push_back(
                    Tier1Server{node_b64(proof.node), candidate.host, candidate.port});
                continue;
            }
            note("proof " + std::string(sec::endpoint_proof_check_name(check)));
            if (check == sec::EndpointProofCheck::NewerEpoch && rewalks < kMaxRewalks) {
                // The verified membership is behind this member. Walk further
                // before trusting anyone, then probe every candidate again.
                ++rewalks;
                std::string ignored;
                bool ignored_hint = false;
                if (state_->walk_from(candidate, ignored, ignored_hint) == Walk::Advanced) {
                    again = true;
                    break;
                }
                continue;
            }
            spdlog::debug("[Tier1Directory] {}:{} is not a Tier 1 endpoint: {}", candidate.host,
                          candidate.port, sec::endpoint_proof_check_name(check));
        }
    }
    if (verified.empty()) {
        // The sources and every probed candidate are named so an outage
        // report shows which servers were asked and what they answered.
        return fail("no verified Tier 1 server is reachable" + tried_detail() +
                        notes_detail("; probed: ", probe_notes),
                    0);
    }

    Tier1Membership membership;
    membership.epoch = state_->walker->epoch().underlying();
    membership.chain = nexus::crypto::to_hex(chain);
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
        state_->known_chain = chain;
        state_->refounded = false;
    }
    spdlog::info("[Tier1Directory] verified Tier 1 epoch {}: {} member(s), {} reachable server(s)",
                 membership.epoch, membership.members.size(), membership.servers.size());
    result.ok = true;
    result.value = std::move(membership);
    return result;
}

Tier1WalkProgress Tier1Directory::walk_progress() const {
    Tier1WalkProgress p;
    p.active = state_->walk_active.load();
    p.epoch = state_->walk_epoch.load();
    p.target = state_->walk_target.load();
    p.pages_done = state_->walk_pages.load();
    if (p.target > p.epoch) {
        const auto links = state_->walk_page_links.load();
        const uint64_t per_page = links > 0 ? links : kChainPageLinks;
        p.pages_total = p.pages_done + (p.target - p.epoch + per_page - 1) / per_page;
    } else if (p.target > 0) {
        p.pages_total = p.pages_done;
    }
    return p;
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

std::optional<std::chrono::milliseconds> Tier1Directory::verified_age() const {
    std::lock_guard lock(state_->mutex);
    if (state_->stale || state_->membership.servers.empty()) return std::nullopt;
    return std::chrono::milliseconds(steady_ms() - state_->verified_at_ms);
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
                                              Tier1Server* answered, std::string& error,
                                              const std::set<std::string>& avoid_nodes) {
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
        if (pin_node.empty() && avoid_nodes.contains(server.node_id)) continue;
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

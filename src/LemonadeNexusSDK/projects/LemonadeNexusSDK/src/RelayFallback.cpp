#include <LemonadeNexusSDK/RelayFallback.hpp>

#include <LemonadeNexusSDK/BoringtunMesh.hpp>
#include <LemonadeNexusSDK/RelayChannel.hpp>

#include <LemonadeNexus/Relay/RelayWire.hpp>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <set>

namespace lnsdk {

namespace {

int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string short_key(const std::string& key) {
    return key.substr(0, std::min<std::size_t>(8, key.size()));
}

}  // namespace

RelayFallback::RelayFallback(BoringtunMesh& mesh, TicketSource tickets, CandidateSource candidates)
    : RelayFallback(mesh, std::move(tickets), std::move(candidates), Timing{}) {}

RelayFallback::RelayFallback(BoringtunMesh& mesh, TicketSource tickets, CandidateSource candidates,
                             Timing timing)
    : mesh_(mesh),
      tickets_(std::move(tickets)),
      candidates_(std::move(candidates)),
      timing_(timing) {}

RelayFallback::~RelayFallback() { stop(); }

bool RelayFallback::relay_needed(bool has_direct, int64_t last_handshake, int64_t now_unix,
                                 std::chrono::milliseconds known_for, bool prefer_direct,
                                 const Timing& timing) {
    if (!prefer_direct) return true;
    if (last_handshake > 0 && now_unix - last_handshake < timing.handshake_fresh_sec) return false;
    if (!has_direct) return true;
    return known_for >= timing.direct_grace;
}

RelayFallback::PairedAction RelayFallback::paired_action(int64_t relay_seen, int64_t handshake,
                                                         int64_t now_unix,
                                                         std::chrono::milliseconds paired_for,
                                                         bool direct_answered,
                                                         const Timing& timing) {
    if (direct_answered && handshake > 0 && now_unix - handshake <= timing.handshake_fresh_sec) {
        return PairedAction::PromoteDirect;
    }
    if (relay_seen <= 0) {
        return paired_for > timing.handshake_grace ? PairedAction::AbandonUnproven
                                                   : PairedAction::Wait;
    }
    const auto grace_sec =
        std::chrono::duration_cast<std::chrono::seconds>(timing.handshake_grace).count();
    if (now_unix - relay_seen > timing.handshake_fresh_sec + grace_sec) {
        return PairedAction::AbandonStale;
    }
    return PairedAction::Working;
}

std::map<std::string, std::string> RelayFallback::relayed_peers() const {
    std::lock_guard lock(relayed_mtx_);
    return relayed_;
}

void RelayFallback::schedule_retry(PeerState& state) {
    ++state.failures;
    auto delay = timing_.backoff_base;
    for (uint32_t i = 1; i < state.failures && delay < timing_.backoff_max; ++i) delay *= 2;
    state.next_attempt = Clock::now() + std::min(delay, timing_.backoff_max);
}

void RelayFallback::abandon(const std::string& pubkey, PeerState& state, bool blame_relay) {
    if (state.channel) {
        state.channel->close();
        state.channel.reset();
    }
    if (state.applied) {
        mesh_.clear_peer_relay(pubkey);
        state.applied = false;
        std::lock_guard lock(relayed_mtx_);
        relayed_.erase(pubkey);
    }
    // A handshake from before now came through the path just abandoned.
    state.stale_before = unix_now();
    if (blame_relay && !state.relay_id.empty()) {
        cooldown_[state.relay_id] = Clock::now() + timing_.relay_cooldown;
    }
    schedule_retry(state);
}

void RelayFallback::promote_direct(const std::string& pubkey, PeerState& state,
                                   const std::string& listed_endpoint) {
    if (state.channel) {
        state.channel->close();
        state.channel.reset();
    }
    // The dataplane already sends to the direct address. Keep it there, and
    // keep the handshake that proved it: no abandon time, no backoff.
    mesh_.clear_peer_relay(pubkey, listed_endpoint);
    state.applied = false;
    state.failures = 0;
    state.next_attempt = {};
    {
        std::lock_guard lock(relayed_mtx_);
        relayed_.erase(pubkey);
    }
    spdlog::info("[RelayFallback] peer {} answered on a direct path; relay closed",
                 short_key(pubkey));
}

void RelayFallback::release_unused(const std::string& pubkey, PeerState& state) {
    if (state.channel) {
        state.channel->close();
        state.channel.reset();
    }
    state.failures = 0;
    state.next_attempt = {};
    spdlog::info("[RelayFallback] peer {} answered on a direct path before its relay was in "
                 "use; relay request closed",
                 short_key(pubkey));
}

void RelayFallback::renew(const std::vector<std::string>& pubkeys, bool skip_grace) {
    const auto now = Clock::now();
    const int64_t now_unix = unix_now();
    for (const auto& pubkey : pubkeys) {
        if (pubkey.empty()) continue;
        auto& state = peers_[pubkey];
        state.listed = true;
        state.stale_before = std::max(state.stale_before, now_unix);
        state.direct_since = skip_grace ? now - timing_.direct_grace : now;
        // A retry delay earned while the peer was away (a relay request it
        // never joined) says nothing about its new path.
        state.next_attempt = {};
    }
}

std::vector<RelayCandidate> RelayFallback::usable_candidates(Clock::time_point now) {
    if (candidate_cache_.empty() || now - candidates_at_ > timing_.candidate_ttl) {
        candidate_cache_ = candidates_ ? candidates_() : std::vector<RelayCandidate>{};
        candidates_at_ = now;
    }
    std::erase_if(cooldown_, [now](const auto& entry) { return entry.second <= now; });
    std::vector<RelayCandidate> out;
    std::set<std::string> seen;
    for (const auto& c : candidate_cache_) {
        if (c.relay_id.empty() || c.host.empty() || c.port == 0) continue;
        if (cooldown_.contains(c.relay_id) || !seen.insert(c.relay_id).second) continue;
        out.push_back(c);
    }
    return out;
}

bool RelayFallback::open_channel(PeerState& state, const MeshPeer& peer,
                                 const RelayCandidate& relay) {
    const auto ip = resolve_relay_ipv4(relay.host);
    if (!ip) {
        spdlog::warn("[RelayFallback] relay host {} does not resolve", relay.host);
        cooldown_[relay.relay_id] = Clock::now() + timing_.relay_cooldown;
        return false;
    }
    const auto ticket = tickets_ ? tickets_(relay.relay_id) : Result<RelayTicket>{};
    if (!ticket.ok) {
        spdlog::warn("[RelayFallback] no relay ticket for peer {}: {}", short_key(peer.mesh_pubkey),
                     ticket.error.empty() ? "request failed" : ticket.error);
        return false;
    }
    const auto window = nexus::relay::relay_rendezvous_window(static_cast<uint64_t>(unix_now()));
    const auto rendezvous = mesh_.relay_rendezvous(peer.mesh_pubkey, relay.relay_id, window);
    if (!rendezvous) return false;
    auto allocate = relay_allocate_datagram(*rendezvous, ticket.value, 1);
    const uint16_t dataplane_port = mesh_.bound_port();
    if (allocate.empty() || dataplane_port == 0) return false;

    RelayChannel::Options options;
    options.relay_ipv4 = *ip;
    options.relay_port = relay.port;
    options.dataplane_port = dataplane_port;
    options.allocate = std::move(allocate);
    options.allocate_retry = timing_.allocate_retry;
    options.heartbeat_interval = timing_.heartbeat_interval;
    options.pending_limit = timing_.pending_limit;
    auto channel = std::make_unique<RelayChannel>();
    if (!channel->open(std::move(options))) return false;

    state.channel = std::move(channel);
    state.relay_id = relay.relay_id;
    state.opened_unix = unix_now();
    spdlog::info("[RelayFallback] peer {} has no direct path; allocating at relay {}",
                 short_key(peer.mesh_pubkey), state.channel->relay_endpoint());
    return true;
}

std::chrono::steady_clock::time_point RelayFallback::maintain(const std::vector<MeshPeer>& peers,
                                                              bool prefer_direct) {
    const auto now = Clock::now();
    const int64_t now_unix = unix_now();
    auto next_due = now + kMaxPassGap;
    const auto own = mesh_.public_key();
    if (own.empty()) {
        stop();
        return next_due;
    }

    std::map<std::string, int64_t> handshakes;
    std::map<std::string, int64_t> last_rx;
    std::map<std::string, std::string> endpoints;  // where the dataplane sends now
    // Peers with a relay path whose traffic now comes from another address
    // (mesh_status reports it as a direct endpoint).
    std::set<std::string> moved_off_relay;
    for (const auto& p : mesh_.mesh_status().peers) {
        handshakes[p.mesh_pubkey] = p.last_handshake;
        last_rx[p.mesh_pubkey] = p.last_rx;
        endpoints[p.mesh_pubkey] = p.endpoint;
        if (!p.endpoint.empty() && p.relay_endpoint.empty()) moved_off_relay.insert(p.mesh_pubkey);
    }

    // Forget peers the server no longer lists.
    std::set<std::string> listed;
    for (const auto& p : peers) {
        if (!p.mesh_pubkey.empty()) listed.insert(p.mesh_pubkey);
    }
    for (auto it = peers_.begin(); it != peers_.end();) {
        if (listed.contains(it->first)) {
            ++it;
            continue;
        }
        abandon(it->first, it->second, false);
        it = peers_.erase(it);
    }

    std::size_t open_count = 0;
    for (const auto& [key, state] : peers_) {
        if (state.channel) ++open_count;
    }
    std::size_t opened = 0;
    std::optional<std::vector<RelayCandidate>> candidates;

    for (const auto& peer : peers) {
        const auto& pubkey = peer.mesh_pubkey;
        if (pubkey.empty() || pubkey == own) continue;
        auto& state = peers_[pubkey];
        if (!state.listed) {
            state.listed = true;
            state.direct_since = now;
        }
        const auto found = handshakes.find(pubkey);
        // The dataplane reports a handshake as now minus whole seconds
        // since it, so one handshake can read a second earlier on a later
        // pass. The newest reading keeps a handshake that once counted as
        // newer than stale_before from dropping back to it.
        state.handshake_seen =
            std::max(state.handshake_seen, found == handshakes.end() ? 0 : found->second);
        const int64_t handshake = state.handshake_seen;

        if (state.channel && !state.applied && prefer_direct) {
            // A handshake since the channel opened, from an address other
            // than the channel's own port, came over a direct path: the
            // relay is not needed. A handshake that the relay already
            // carried to the channel's port means the peer uses the relay.
            const auto at = endpoints.find(pubkey);
            const bool direct = at != endpoints.end() && !at->second.empty() &&
                                at->second != state.channel->local_endpoint();
            if (direct && handshake > 0 && handshake >= state.opened_unix &&
                handshake > state.stale_before) {
                release_unused(pubkey, state);
                continue;
            }
        }

        if (state.channel) {
            if (!state.applied || state.relay_seen == 0) {
                next_due = std::min(next_due, now + kPendingPassGap);
            }
            switch (state.channel->state()) {
                case RelayChannel::State::Paired:
                    if (!state.applied) {
                        const auto relay_endpoint = state.channel->relay_endpoint();
                        if (mesh_.set_peer_relay(pubkey, state.channel->local_endpoint(),
                                                 relay_endpoint)) {
                            state.applied = true;
                            state.paired_at = now;
                            state.paired_unix = now_unix;
                            state.relay_seen = 0;
                            std::lock_guard lock(relayed_mtx_);
                            relayed_[pubkey] = relay_endpoint;
                        } else {
                            abandon(pubkey, state, false);
                        }
                    } else {
                        // The relay must carry traffic soon after pairing and
                        // keep carrying it. A handshake or an authenticated
                        // packet (keepalives included) since pairing proves
                        // it: a session that predates the pairing renews no
                        // handshake through the new relay for up to two
                        // minutes, and an idle session reads no handshake at
                        // all while it renegotiates after REJECT_AFTER_TIME.
                        // Treating either as a dead relay re-allocated a
                        // working relay and blamed it, so the two sides chose
                        // different relays and kept re-allocating.
                        const bool moved = moved_off_relay.contains(pubkey);
                        const auto rx = last_rx.find(pubkey);
                        for (const int64_t seen :
                             {handshake, rx == last_rx.end() ? int64_t{0} : rx->second}) {
                            if (!moved && seen > 0 && seen >= state.paired_unix &&
                                seen > state.relay_seen) {
                                state.relay_seen = seen;
                            }
                        }
                        const int64_t since_pair =
                            handshake > 0 && handshake >= state.paired_unix ? handshake : 0;
                        switch (paired_action(
                            state.relay_seen, since_pair, now_unix,
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                now - state.paired_at),
                            prefer_direct && !peer.endpoint.empty() && moved, timing_)) {
                            case PairedAction::PromoteDirect:
                                // A punched direct path answered with an
                                // authenticated handshake: it replaces the relay.
                                promote_direct(pubkey, state, peer.endpoint);
                                break;
                            case PairedAction::AbandonUnproven:
                                spdlog::warn("[RelayFallback] no traffic with peer {} through relay",
                                             short_key(pubkey));
                                abandon(pubkey, state, true);
                                break;
                            case PairedAction::AbandonStale:
                                abandon(pubkey, state, false);
                                break;
                            case PairedAction::Working:
                                state.failures = 0;
                                break;
                            case PairedAction::Wait:
                                break;
                        }
                    }
                    break;
                case RelayChannel::State::Failed: {
                    const auto why = state.channel->failure();
                    abandon(pubkey, state,
                            why == RelayChannel::Failure::NoReply ||
                                why == RelayChannel::Failure::Refused ||
                                why == RelayChannel::Failure::RelaySilent);
                    break;
                }
                default:
                    break;  // Allocating or Waiting.
            }
            continue;
        }

        const int64_t usable_handshake = handshake > state.stale_before ? handshake : 0;
        const auto known_for =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - state.direct_since);
        if (!relay_needed(!peer.endpoint.empty(), usable_handshake, now_unix, known_for,
                          prefer_direct, timing_)) {
            // Come back when the grace period ends, unless a working path
            // makes the relay unnecessary anyway.
            const bool working = prefer_direct && usable_handshake > 0 &&
                                 now_unix - usable_handshake < timing_.handshake_fresh_sec;
            if (!working) next_due = std::min(next_due, state.direct_since + timing_.direct_grace);
            continue;
        }
        if (now < state.next_attempt) {
            next_due = std::min(next_due, state.next_attempt);
            continue;
        }
        if (opened >= kMaxOpensPerPass || open_count >= kMaxChannels) {
            next_due = std::min(next_due, now + kPendingPassGap);
            continue;
        }

        if (!candidates) candidates = usable_candidates(now);
        std::vector<std::string> ids;
        ids.reserve(candidates->size());
        for (const auto& c : *candidates) ids.push_back(c.relay_id);
        const auto choice = select_relay_for_pair(ids, own, pubkey);
        if (!choice) {
            schedule_retry(state);
            continue;
        }
        ++opened;
        if (open_channel(state, peer, (*candidates)[*choice])) {
            ++open_count;
            next_due = std::min(next_due, now + kPendingPassGap);
        } else {
            schedule_retry(state);
            next_due = std::min(next_due, state.next_attempt);
        }
    }
    return std::max(next_due, now + kMinPassGap);
}

void RelayFallback::stop() {
    for (auto& [pubkey, state] : peers_) {
        if (state.channel) state.channel->close();
        if (state.applied) mesh_.clear_peer_relay(pubkey);
    }
    peers_.clear();
    std::lock_guard lock(relayed_mtx_);
    relayed_.clear();
}

}  // namespace lnsdk

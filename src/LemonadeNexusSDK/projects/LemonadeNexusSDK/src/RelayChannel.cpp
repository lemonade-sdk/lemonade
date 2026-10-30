#include <LemonadeNexusSDK/RelayChannel.hpp>

#include <LemonadeNexus/Relay/RelayWire.hpp>
#include <LemonadeNexus/Security/CanonicalEncoding.hpp>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
using socket_t = SOCKET;
static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
static void close_socket(socket_t s) { closesocket(s); }
using sock_len_t = int;
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
using socket_t = int;
static constexpr socket_t kInvalidSocket = -1;
static void close_socket(socket_t s) { ::close(s); }
using sock_len_t = socklen_t;
#endif

namespace lnsdk {

namespace relay = nexus::relay;

namespace {

/// A channel pass follows the previous one within the 100 ms receive
/// timeout. A gap longer than this means the process was suspended.
constexpr std::chrono::seconds kSuspendGap{5};

void ensure_sockets() {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    });
#endif
}

bool same_address(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

std::string to_string(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof ip);
    return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
}

void send_to(socket_t sock, const uint8_t* data, std::size_t len, const sockaddr_in& to) {
    sendto(sock, reinterpret_cast<const char*>(data),
#ifdef _WIN32
           static_cast<int>(len),
#else
           len,
#endif
           0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

}  // namespace

struct RelayChannel::Impl {
    Options             opts;
    socket_t            sock{kInvalidSocket};
    sockaddr_in         relay_addr{};
    sockaddr_in         dataplane_addr{};
    uint16_t            local_port{0};
    std::atomic<State>  state{State::Idle};
    std::atomic<Failure> failure{Failure::None};
    std::atomic<bool>   running{false};
    std::thread         thread;
    std::mutex          close_mtx;

    // Written by the channel thread only; read by close() after the join.
    relay::SessionId session{};
    bool             have_session{false};
    uint32_t         sequence{1};

    void fail(Failure why) {
        failure.store(why);
        state.store(State::Failed);
        running.store(false);
    }

    void send_relay(relay::RelayMsgType type, std::span<const uint8_t> payload = {}) {
        const auto datagram = relay::make_relay_datagram(type, session, sequence++, payload);
        if (!datagram.empty()) send_to(sock, datagram.data(), datagram.size(), relay_addr);
    }

    void run();
    void on_relay_datagram(std::span<const uint8_t> datagram,
                           std::chrono::steady_clock::time_point now,
                           std::chrono::steady_clock::time_point& allocated_at,
                           std::chrono::steady_clock::time_point& next_heartbeat);
};

void RelayChannel::Impl::on_relay_datagram(std::span<const uint8_t> datagram,
                                           std::chrono::steady_clock::time_point now,
                                           std::chrono::steady_clock::time_point& allocated_at,
                                           std::chrono::steady_clock::time_point& next_heartbeat) {
    const State current = state.load();
    const bool control = datagram.size() >= 2 && datagram[0] == 0x52 && datagram[1] == 0x4C;
    if (!control) {
        // The relay forwards the peer's BoringTun datagram without a header,
        // and only after both sides are bound: it also proves the pairing
        // when the Bound message was lost.
        if (current == State::Waiting) state.store(State::Paired);
        if (current == State::Waiting || current == State::Paired) {
            send_to(sock, datagram.data(), datagram.size(), dataplane_addr);
        }
        return;
    }

    const auto header = relay::decode_relay_header(datagram);
    if (!header) return;
    const auto payload = datagram.subspan(relay::kRelayHeaderSize, header->payload_length);
    const bool ours = have_session && header->session_id == session;

    switch (header->msg_type) {
        case relay::RelayMsgType::Allocated: {
            const auto reply = relay::decode_relay_allocated(payload);
            if (!reply) return;
            if (current == State::Allocating) {
                session = header->session_id;
                have_session = true;
                allocated_at = now;
                next_heartbeat = now + opts.heartbeat_interval;
            } else if (!(current == State::Waiting && ours)) {
                return;
            }
            state.store(reply->state == relay::RelayAllocationState::Paired ? State::Paired
                                                                            : State::Waiting);
            break;
        }
        case relay::RelayMsgType::Bound:
            if (ours && current == State::Waiting) state.store(State::Paired);
            break;
        case relay::RelayMsgType::Error: {
            const uint8_t code = payload.empty() ? 0 : payload[0];
            if (current == State::Allocating && header->session_id == relay::SessionId{}) {
                spdlog::warn("[RelayChannel] relay {} refused the allocation (code {})",
                             to_string(relay_addr), code);
                fail(Failure::Refused);
            } else if (ours) {
                spdlog::info("[RelayChannel] relay {} ended the session (code {})",
                             to_string(relay_addr), code);
                fail(Failure::SessionLost);
            }
            break;
        }
        default:
            break;  // HeartbeatAck: the receive time is all that matters.
    }
}

void RelayChannel::Impl::run() {
    using clock = std::chrono::steady_clock;
    auto next_allocate = clock::now();
    auto allocated_at = clock::time_point{};
    auto next_heartbeat = clock::time_point{};
    auto last_relay_rx = clock::now();
    auto last_pass = clock::now();
    uint32_t attempts = 0;

    std::vector<uint8_t> buf(65536);
    std::vector<uint8_t> wrapped;

    while (running.load()) {
        const auto now = clock::now();
        const State current = state.load();

        // The relay could not reach a suspended process (an app in the
        // background, a stopped process): its silence during the gap says
        // nothing about the relay. Blaming it put the relay in cooldown on
        // this side only, so the two sides chose different relays. Restart
        // the silence and pending windows and send a heartbeat now.
        if (const auto gap = now - last_pass; gap > kSuspendGap) {
            last_relay_rx = now;
            allocated_at += gap;
            next_heartbeat = now;
        }
        last_pass = now;

        if (current == State::Allocating && now >= next_allocate) {
            if (attempts >= opts.allocate_attempts) {
                spdlog::warn("[RelayChannel] relay {} did not answer", to_string(relay_addr));
                fail(Failure::NoReply);
                break;
            }
            send_to(sock, opts.allocate.data(), opts.allocate.size(), relay_addr);
            ++attempts;
            next_allocate = now + opts.allocate_retry;
        }
        if (current == State::Waiting && now - allocated_at > opts.pending_limit) {
            fail(Failure::PeerAbsent);
            break;
        }
        if (current == State::Waiting || current == State::Paired) {
            if (now - last_relay_rx > opts.relay_silence_limit) {
                spdlog::warn("[RelayChannel] relay {} went silent", to_string(relay_addr));
                fail(Failure::RelaySilent);
                break;
            }
            if (now >= next_heartbeat) {
                send_relay(relay::RelayMsgType::Heartbeat);
                next_heartbeat = now + opts.heartbeat_interval;
            }
        }

        sockaddr_in from{};
        sock_len_t from_len = sizeof(from);
        const auto n = recvfrom(sock, reinterpret_cast<char*>(buf.data()),
#ifdef _WIN32
                                static_cast<int>(buf.size()),
#else
                                buf.size(),
#endif
                                0, reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n <= 0) continue;  // timeout, shutdown, or a transient error
        const std::span<const uint8_t> datagram(buf.data(), static_cast<std::size_t>(n));

        if (same_address(from, relay_addr)) {
            last_relay_rx = clock::now();
            on_relay_datagram(datagram, last_relay_rx, allocated_at, next_heartbeat);
        } else if (same_address(from, dataplane_addr)) {
            // While waiting, send anyway: the relay drops it until both sides
            // are bound, and a lost Bound message then costs nothing.
            const State st = state.load();
            if ((st == State::Waiting || st == State::Paired) &&
                datagram.size() <= 0xFFFF) {
                wrapped = relay::make_relay_datagram(relay::RelayMsgType::Data, session,
                                                     sequence++, datagram);
                send_to(sock, wrapped.data(), wrapped.size(), relay_addr);
            }
        }
        // Any other sender is dropped.
    }
}

RelayChannel::RelayChannel() : impl_(std::make_unique<Impl>()) {}

RelayChannel::~RelayChannel() { close(); }

bool RelayChannel::open(Options options) {
    if (impl_->state.load() != State::Idle) return false;
    if (options.allocate.empty() || options.dataplane_port == 0 || options.relay_port == 0 ||
        options.allocate_attempts == 0) {
        return false;
    }
    sockaddr_in relay_addr{};
    relay_addr.sin_family = AF_INET;
    relay_addr.sin_port = htons(options.relay_port);
    if (inet_pton(AF_INET, options.relay_ipv4.c_str(), &relay_addr.sin_addr) != 1) return false;

    ensure_sockets();
    const socket_t sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == kInvalidSocket) return false;
#ifdef _WIN32
    DWORD timeout_ms = 100;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms),
               sizeof(timeout_ms));
#else
    timeval tv{};
    tv.tv_usec = 100 * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    sockaddr_in bind_addr{};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = 0;
    if (bind(sock, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)) != 0) {
        close_socket(sock);
        return false;
    }
    sockaddr_in actual{};
    sock_len_t actual_len = sizeof(actual);
    getsockname(sock, reinterpret_cast<sockaddr*>(&actual), &actual_len);

    impl_->opts = std::move(options);
    impl_->sock = sock;
    impl_->relay_addr = relay_addr;
    impl_->dataplane_addr.sin_family = AF_INET;
    impl_->dataplane_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    impl_->dataplane_addr.sin_port = htons(impl_->opts.dataplane_port);
    impl_->local_port = ntohs(actual.sin_port);
    impl_->state.store(State::Allocating);
    impl_->running.store(true);
    impl_->thread = std::thread([impl = impl_.get()] { impl->run(); });
    return true;
}

void RelayChannel::close() {
    std::lock_guard lock(impl_->close_mtx);
    impl_->running.store(false);
    if (impl_->thread.joinable()) impl_->thread.join();
    if (impl_->sock == kInvalidSocket) return;
    // Free the relay's session now instead of waiting for its idle timeout.
    if (impl_->have_session) impl_->send_relay(relay::RelayMsgType::Teardown);
    close_socket(impl_->sock);
    impl_->sock = kInvalidSocket;
}

RelayChannel::State RelayChannel::state() const { return impl_->state.load(); }

RelayChannel::Failure RelayChannel::failure() const { return impl_->failure.load(); }

std::string RelayChannel::local_endpoint() const {
    if (impl_->local_port == 0) return {};
    return "127.0.0.1:" + std::to_string(impl_->local_port);
}

std::string RelayChannel::relay_endpoint() const {
    return impl_->opts.relay_ipv4 + ":" + std::to_string(impl_->opts.relay_port);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::vector<uint8_t> relay_allocate_datagram(const std::array<uint8_t, 64>& rendezvous,
                                             const RelayTicket& ticket, uint32_t sequence) {
    relay::RelayRendezvous material;
    std::memcpy(material.id.data(), rendezvous.data(), 16);
    std::memcpy(material.own_proof.data(), rendezvous.data() + 16, 16);
    std::memcpy(material.peer_commitment.data(), rendezvous.data() + 32, 32);
    // The same JSON form that POST /api/relay/ticket returns.
    const nlohmann::json doc{
        {"peer_id", ticket.peer_id},       {"relay_id", ticket.relay_id},
        {"session_nonce", ticket.session_nonce}, {"issued_at", ticket.issued_at},
        {"expires_at", ticket.expires_at}, {"network_id", ticket.network_id},
        {"epoch", ticket.epoch},           {"issuer", ticket.issuer},
        {"signature", ticket.signature},
    };
    return relay::make_relay_allocate(material, doc.dump(), sequence);
}

std::optional<std::size_t> select_relay_for_pair(const std::vector<std::string>& relay_ids,
                                                 const std::string& pubkey_a,
                                                 const std::string& pubkey_b) {
    const auto& lo = pubkey_a < pubkey_b ? pubkey_a : pubkey_b;
    const auto& hi = pubkey_a < pubkey_b ? pubkey_b : pubkey_a;
    std::optional<std::size_t> best;
    nexus::security::Digest best_score{};
    for (std::size_t i = 0; i < relay_ids.size(); ++i) {
        nexus::security::CanonicalEncoder encoder("lemonade-nexus/relay-select:v1");
        encoder.add_string(relay_ids[i]);
        encoder.add_string(lo);
        encoder.add_string(hi);
        const auto score = encoder.digest();
        if (!best || score > best_score ||
            (score == best_score && relay_ids[i] < relay_ids[*best])) {
            best = i;
            best_score = score;
        }
    }
    return best;
}

std::optional<std::string> resolve_relay_ipv4(const std::string& host) {
    if (host.empty()) return std::nullopt;
    ensure_sockets();
    in_addr literal{};
    if (inet_pton(AF_INET, host.c_str(), &literal) == 1) return host;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return std::nullopt;
    char buf[INET_ADDRSTRLEN] = {};
    const auto* sin = reinterpret_cast<const sockaddr_in*>(res->ai_addr);
    inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof buf);
    freeaddrinfo(res);
    return std::string(buf);
}

}  // namespace lnsdk

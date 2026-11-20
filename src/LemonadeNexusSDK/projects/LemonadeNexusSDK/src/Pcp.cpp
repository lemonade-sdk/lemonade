#include <LemonadeNexusSDK/Pcp.hpp>

#include "UdpExchange.hpp"

#include <sodium.h>

#include <algorithm>
#include <cstring>
#include <string>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#endif

namespace lnsdk::pcp {

namespace {

using namespace std::chrono_literals;

constexpr std::array<std::chrono::milliseconds, 3> kFirstWaits{1000ms, 2000ms, 4000ms};
constexpr std::array<std::chrono::milliseconds, 2> kRenewWaits{1000ms, 1500ms};

/// Offsets in a MAP request and response (RFC 6887 sections 7.1, 7.2, 11.1).
constexpr size_t kOffOpcode = 1;
constexpr size_t kOffResult = 3;
constexpr size_t kOffLifetime = 4;
constexpr size_t kOffClientAddress = 8;
constexpr size_t kOffEpoch = 8;
constexpr size_t kOffNonce = 24;
constexpr size_t kOffProtocol = 36;
constexpr size_t kOffInternalPort = 40;
constexpr size_t kOffExternalPort = 42;
constexpr size_t kOffExternalAddress = 44;
constexpr size_t kAddressLen = 16;

void put16(std::vector<uint8_t>& pkt, size_t at, uint16_t value) {
    pkt[at] = static_cast<uint8_t>(value >> 8);
    pkt[at + 1] = static_cast<uint8_t>(value & 0xFF);
}

void put32(std::vector<uint8_t>& pkt, size_t at, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) {
        pkt[at + i] = static_cast<uint8_t>(value >> (8 * (3 - i)));
    }
}

/// Writes `address` as ::ffff:a.b.c.d, or 16 zero bytes when it is 0.
void put_ipv4_mapped(std::vector<uint8_t>& pkt, size_t at, uint32_t address) {
    if (address == 0) return;
    pkt[at + 10] = 0xFF;
    pkt[at + 11] = 0xFF;
    put32(pkt, at + 12, address);
}

uint16_t get16(std::span<const uint8_t> bytes, size_t at) {
    return static_cast<uint16_t>((uint16_t{bytes[at]} << 8) | bytes[at + 1]);
}

uint32_t get32(std::span<const uint8_t> bytes, size_t at) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) value = (value << 8) | bytes[at + i];
    return value;
}

bool all_zero(std::span<const uint8_t> bytes) {
    return std::all_of(bytes.begin(), bytes.end(), [](uint8_t b) { return b == 0; });
}

/// The IPv4 address inside an IPv4-mapped IPv6 address at `at`, nullopt for
/// any other 16-byte address.
std::optional<uint32_t> get_ipv4_mapped(std::span<const uint8_t> bytes, size_t at) {
    if (!all_zero(bytes.subspan(at, 10)) || bytes[at + 10] != 0xFF || bytes[at + 11] != 0xFF) {
        return std::nullopt;
    }
    return get32(bytes, at + 12);
}

template <typename T>
Result<T> fail(std::string error) {
    Result<T> r;
    r.error = std::move(error);
    return r;
}

template <typename T>
Result<T> succeed(T value) {
    Result<T> r;
    r.ok = true;
    r.value = std::move(value);
    return r;
}

}  // namespace

const char* result_code_name(ResultCode code) noexcept {
    switch (code) {
        case ResultCode::Success: return "Success";
        case ResultCode::UnsuppVersion: return "UnsuppVersion";
        case ResultCode::NotAuthorized: return "NotAuthorized";
        case ResultCode::MalformedRequest: return "MalformedRequest";
        case ResultCode::UnsuppOpcode: return "UnsuppOpcode";
        case ResultCode::UnsuppOption: return "UnsuppOption";
        case ResultCode::MalformedOption: return "MalformedOption";
        case ResultCode::NetworkFailure: return "NetworkFailure";
        case ResultCode::NoResources: return "NoResources";
        case ResultCode::UnsuppProtocol: return "UnsuppProtocol";
        case ResultCode::UserExQuota: return "UserExQuota";
        case ResultCode::CannotProvideExternal: return "CannotProvideExternal";
        case ResultCode::AddressMismatch: return "AddressMismatch";
        case ResultCode::ExcessiveRemotePeers: return "ExcessiveRemotePeers";
    }
    return "Unknown";
}

std::vector<uint8_t> encode_map_request(const MapRequest& request) {
    std::vector<uint8_t> pkt(kMapRequestLen, 0);
    pkt[0] = kVersion;
    pkt[kOffOpcode] = kOpcodeMap;
    put32(pkt, kOffLifetime, request.lifetime_sec);
    put_ipv4_mapped(pkt, kOffClientAddress, request.client_address);
    std::copy(request.nonce.begin(), request.nonce.end(), pkt.begin() + kOffNonce);
    pkt[kOffProtocol] = request.protocol;
    put16(pkt, kOffInternalPort, request.internal_port);
    put16(pkt, kOffExternalPort, request.suggested_external_port);
    put_ipv4_mapped(pkt, kOffExternalAddress, request.suggested_external_address);
    return pkt;
}

std::optional<MapResponse> decode_map_response(std::span<const uint8_t> bytes) {
    if (bytes.size() != kMapResponseLen && bytes.size() != kHeaderLen) return std::nullopt;
    if (bytes[0] != kVersion || bytes[kOffOpcode] != (kOpcodeMap | kResponseBit)) {
        return std::nullopt;
    }
    MapResponse out;
    out.result = static_cast<ResultCode>(bytes[kOffResult]);
    out.lifetime_sec = get32(bytes, kOffLifetime);
    out.epoch_sec = get32(bytes, kOffEpoch);
    if (bytes.size() == kHeaderLen) {
        if (out.result == ResultCode::Success) return std::nullopt;
        return out;
    }
    std::copy_n(bytes.begin() + kOffNonce, kNonceLen, out.nonce.begin());
    out.protocol = bytes[kOffProtocol];
    out.internal_port = get16(bytes, kOffInternalPort);
    out.external_port = get16(bytes, kOffExternalPort);
    const auto address = get_ipv4_mapped(bytes, kOffExternalAddress);
    if (address) {
        out.external_address = *address;
    } else if (out.result == ResultCode::Success ||
               !all_zero(bytes.subspan(kOffExternalAddress, kAddressLen))) {
        return std::nullopt;
    }
    return out;
}

std::optional<uint8_t> decode_unsupported_version(std::span<const uint8_t> bytes) {
    if (bytes.size() < 4 || bytes[0] == kVersion) return std::nullopt;
    if (bytes[kOffResult] != static_cast<uint8_t>(ResultCode::UnsuppVersion)) return std::nullopt;
    return bytes[0];
}

bool epoch_indicates_restart(uint32_t previous_epoch, int64_t previous_time_unix,
                             uint32_t current_epoch, int64_t now_unix) {
    if (current_epoch < previous_epoch) return true;
    const int64_t client_delta = std::max<int64_t>(0, now_unix - previous_time_unix);
    const int64_t server_delta = int64_t{current_epoch} - int64_t{previous_epoch};
    return client_delta + 2 < server_delta - server_delta / 16 ||
           server_delta + 2 < client_delta - client_delta / 16;
}

PcpClient::PcpClient(std::string gateway_ipv4, std::string local_ipv4, uint16_t port,
                     const std::atomic<bool>* cancel)
    : gateway_(std::move(gateway_ipv4)), local_(std::move(local_ipv4)), port_(port),
      cancel_(cancel) {}

std::optional<ResultCode> PcpClient::last_result_code() const noexcept {
    return last_result_code_;
}

uint32_t PcpClient::last_epoch_sec() const noexcept { return last_epoch_sec_; }

std::optional<uint8_t> PcpClient::unsupported_version() const noexcept {
    return unsupported_version_;
}

std::optional<Nonce> PcpClient::nonce() const noexcept {
    if (!held_) return std::nullopt;
    return held_->nonce;
}

Result<MapResponse> PcpClient::map_udp(uint16_t internal_port, uint16_t suggested_external_port,
                                       uint32_t lifetime_sec) {
    if (internal_port == 0) return fail<MapResponse>("PCP internal port must not be 0");
    if (lifetime_sec == 0) return fail<MapResponse>("PCP mapping lifetime must not be 0");
    if (!held_ || held_->internal_port != internal_port) {
        Held fresh;
        fresh.internal_port = internal_port;
        randombytes_buf(fresh.nonce.data(), fresh.nonce.size());
        held_ = fresh;
    }
    MapRequest request;
    request.lifetime_sec = lifetime_sec;
    request.nonce = held_->nonce;
    request.internal_port = internal_port;
    request.suggested_external_port = suggested_external_port;
    auto r = exchange(request, kFirstWaits);
    if (r) held_->external_port = r.value.external_port;
    return r;
}

Result<MapResponse> PcpClient::renew_udp(uint32_t lifetime_sec) {
    if (lifetime_sec == 0) return fail<MapResponse>("PCP mapping lifetime must not be 0");
    if (!held_) return fail<MapResponse>("PCP client holds no mapping to renew");
    MapRequest request;
    request.lifetime_sec = lifetime_sec;
    request.nonce = held_->nonce;
    request.internal_port = held_->internal_port;
    request.suggested_external_port = held_->external_port;
    auto r = exchange(request, kRenewWaits);
    if (r) held_->external_port = r.value.external_port;
    return r;
}

StatusResult PcpClient::release_udp(uint16_t internal_port) {
    if (internal_port == 0) return fail<std::monostate>("PCP internal port must not be 0");
    if (!held_ || held_->internal_port != internal_port) {
        return fail<std::monostate>("PCP client holds no mapping for that port");
    }
    MapRequest request;
    request.nonce = held_->nonce;
    request.internal_port = internal_port;
    auto r = exchange(request, kRenewWaits);
    if (!r) return fail<std::monostate>(std::move(r.error));
    held_.reset();
    return succeed(std::monostate{});
}

Result<MapResponse> PcpClient::exchange(const MapRequest& request,
                                        std::span<const std::chrono::milliseconds> waits) {
    MapRequest on_wire = request;
    in_addr local{};
    if (inet_pton(AF_INET, local_.c_str(), &local) != 1) {
        return fail<MapResponse>("PCP local address is not a dotted IPv4 address");
    }
    uint32_t local_be = 0;
    std::memcpy(&local_be, &local.s_addr, sizeof(local_be));
    on_wire.client_address = ntohl(local_be);
    const auto datagram = encode_map_request(on_wire);

    std::optional<MapResponse> accepted;
    std::optional<uint8_t> fallback;
    const auto accept = [&](std::span<const uint8_t> bytes) {
        if (const auto version = decode_unsupported_version(bytes)) {
            fallback = version;
            return true;
        }
        auto parsed = decode_map_response(bytes);
        if (!parsed) return false;
        const bool header_only = bytes.size() == kHeaderLen;
        if (!header_only && (parsed->nonce != request.nonce ||
                             parsed->internal_port != request.internal_port ||
                             parsed->protocol != request.protocol)) {
            return false;
        }
        accepted = std::move(parsed);
        return true;
    };
    auto reply = udp_exchange::exchange(gateway_, port_, local_, datagram, waits, accept, "PCP",
                                        cancel_);
    if (!reply) return fail<MapResponse>(std::move(reply.error));
    if (fallback) {
        unsupported_version_ = fallback;
        last_result_code_ = ResultCode::UnsuppVersion;
        return fail<MapResponse>("PCP gateway supports only version " +
                                 std::to_string(unsigned{*fallback}) + "; try NAT-PMP");
    }
    last_result_code_ = accepted->result;
    last_epoch_sec_ = accepted->epoch_sec;
    if (accepted->result != ResultCode::Success) {
        return fail<MapResponse>("PCP gateway answered result code " +
                                 std::to_string(unsigned{static_cast<uint8_t>(accepted->result)}) +
                                 " (" + result_code_name(accepted->result) + ")");
    }
    return succeed(std::move(*accepted));
}

}  // namespace lnsdk::pcp

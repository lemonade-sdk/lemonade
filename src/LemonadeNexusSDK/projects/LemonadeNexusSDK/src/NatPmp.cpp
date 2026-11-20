#include <LemonadeNexusSDK/NatPmp.hpp>

#include "UdpExchange.hpp"

#include <algorithm>
#include <array>
#include <chrono>

namespace lnsdk::natpmp {

namespace {

using namespace std::chrono_literals;

constexpr std::array<std::chrono::milliseconds, 4> kWaits{250ms, 500ms, 1000ms, 2000ms};

/// Offsets shared by every response: version, opcode, result code, epoch.
constexpr size_t kOffOpcode = 1;
constexpr size_t kOffResult = 2;
constexpr size_t kOffEpoch = 4;
constexpr size_t kMinResponseLen = 8;

void put16(std::vector<uint8_t>& pkt, size_t at, uint16_t value) {
    pkt[at] = static_cast<uint8_t>(value >> 8);
    pkt[at + 1] = static_cast<uint8_t>(value & 0xFF);
}

void put32(std::vector<uint8_t>& pkt, size_t at, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) {
        pkt[at + i] = static_cast<uint8_t>(value >> (8 * (3 - i)));
    }
}

uint16_t get16(std::span<const uint8_t> bytes, size_t at) {
    return static_cast<uint16_t>((uint16_t{bytes[at]} << 8) | bytes[at + 1]);
}

uint32_t get32(std::span<const uint8_t> bytes, size_t at) {
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i) value = (value << 8) | bytes[at + i];
    return value;
}

template <typename T>
Result<T> fail(std::string error) {
    Result<T> r;
    r.error = std::move(error);
    return r;
}

/// Whether `bytes` is a response of the expected shape for `request_opcode`:
/// a success response of `response_len` bytes, or an error response (at
/// least 8 bytes) with the response opcode or, for an unsupported version,
/// opcode 0.
bool answers(std::span<const uint8_t> bytes, uint8_t request_opcode, size_t response_len) {
    if (bytes.size() < kMinResponseLen || bytes[0] != kVersion) return false;
    const uint8_t opcode = bytes[kOffOpcode];
    const uint16_t result = get16(bytes, kOffResult);
    if (opcode == static_cast<uint8_t>(request_opcode + kResponseBit)) {
        return result != 0 || bytes.size() == response_len;
    }
    return opcode == 0 && result == static_cast<uint16_t>(ResultCode::UnsupportedVersion);
}

}  // namespace

const char* result_code_name(ResultCode code) noexcept {
    switch (code) {
        case ResultCode::Success: return "Success";
        case ResultCode::UnsupportedVersion: return "UnsupportedVersion";
        case ResultCode::NotAuthorized: return "NotAuthorized";
        case ResultCode::NetworkFailure: return "NetworkFailure";
        case ResultCode::OutOfResources: return "OutOfResources";
        case ResultCode::UnsupportedOpcode: return "UnsupportedOpcode";
    }
    return "Unknown";
}

std::vector<uint8_t> encode_external_address_request() {
    return {kVersion, kOpExternalAddress};
}

std::vector<uint8_t> encode_udp_map_request(uint16_t internal_port,
                                            uint16_t suggested_external_port,
                                            uint32_t lifetime_sec) {
    std::vector<uint8_t> pkt(kMapRequestLen, 0);
    pkt[0] = kVersion;
    pkt[1] = kOpMapUdp;
    put16(pkt, 4, internal_port);
    put16(pkt, 6, suggested_external_port);
    put32(pkt, 8, lifetime_sec);
    return pkt;
}

std::optional<ResultCode> decode_result_code(std::span<const uint8_t> bytes) {
    if (bytes.size() < 4 || bytes[0] != kVersion) return std::nullopt;
    return static_cast<ResultCode>(get16(bytes, kOffResult));
}

std::optional<ExternalAddress> decode_external_address(std::span<const uint8_t> bytes) {
    if (bytes.size() != kExternalAddressResponseLen || bytes[0] != kVersion) return std::nullopt;
    if (bytes[kOffOpcode] != kOpExternalAddress + kResponseBit) return std::nullopt;
    if (get16(bytes, kOffResult) != static_cast<uint16_t>(ResultCode::Success)) {
        return std::nullopt;
    }
    return ExternalAddress{get32(bytes, kOffEpoch), get32(bytes, 8)};
}

std::optional<Mapping> decode_udp_mapping(std::span<const uint8_t> bytes) {
    if (bytes.size() != kMapResponseLen || bytes[0] != kVersion) return std::nullopt;
    if (bytes[kOffOpcode] != kOpMapUdp + kResponseBit) return std::nullopt;
    if (get16(bytes, kOffResult) != static_cast<uint16_t>(ResultCode::Success)) {
        return std::nullopt;
    }
    return Mapping{get32(bytes, kOffEpoch), get16(bytes, 8), get16(bytes, 10), get32(bytes, 12)};
}

bool epoch_indicates_restart(uint32_t previous_epoch, int64_t previous_time_unix,
                             uint32_t current_epoch, int64_t now_unix) {
    const int64_t elapsed = std::max<int64_t>(0, now_unix - previous_time_unix);
    const int64_t expected = int64_t{previous_epoch} + elapsed * 7 / 8;
    return int64_t{current_epoch} < expected - 2;
}

NatPmpClient::NatPmpClient(std::string gateway_ipv4, uint16_t port,
                           const std::atomic<bool>* cancel)
    : gateway_(std::move(gateway_ipv4)), port_(port), cancel_(cancel) {}

std::optional<ResultCode> NatPmpClient::last_result_code() const noexcept {
    return last_result_code_;
}

uint32_t NatPmpClient::last_epoch_sec() const noexcept { return last_epoch_sec_; }

Result<ExternalAddress> NatPmpClient::external_address() {
    auto reply = exchange(encode_external_address_request(), kOpExternalAddress,
                          kExternalAddressResponseLen, std::nullopt);
    if (!reply) return fail<ExternalAddress>(std::move(reply.error));
    auto parsed = decode_external_address(reply.value);
    if (!parsed) return fail<ExternalAddress>("NAT-PMP external address response did not decode");
    Result<ExternalAddress> r;
    r.ok = true;
    r.value = *parsed;
    return r;
}

Result<Mapping> NatPmpClient::map_udp(uint16_t internal_port, uint16_t suggested_external_port,
                                      uint32_t lifetime_sec) {
    if (internal_port == 0) return fail<Mapping>("NAT-PMP internal port must not be 0");
    if (lifetime_sec == 0) return fail<Mapping>("NAT-PMP mapping lifetime must not be 0");
    auto reply = exchange(encode_udp_map_request(internal_port, suggested_external_port, lifetime_sec),
                          kOpMapUdp, kMapResponseLen, internal_port);
    if (!reply) return fail<Mapping>(std::move(reply.error));
    auto parsed = decode_udp_mapping(reply.value);
    if (!parsed) return fail<Mapping>("NAT-PMP mapping response did not decode");
    Result<Mapping> r;
    r.ok = true;
    r.value = *parsed;
    return r;
}

StatusResult NatPmpClient::release_udp(uint16_t internal_port) {
    if (internal_port == 0) return fail<std::monostate>("NAT-PMP internal port must not be 0");
    auto reply = exchange(encode_udp_map_request(internal_port, 0, 0), kOpMapUdp, kMapResponseLen,
                          internal_port);
    if (!reply) return fail<std::monostate>(std::move(reply.error));
    StatusResult r;
    r.ok = true;
    return r;
}

Result<std::vector<uint8_t>> NatPmpClient::exchange(const std::vector<uint8_t>& request,
                                                    uint8_t request_opcode, size_t response_len,
                                                    std::optional<uint16_t> internal_port) {
    const auto accept = [&](std::span<const uint8_t> bytes) {
        if (!answers(bytes, request_opcode, response_len)) return false;
        const auto result = static_cast<ResultCode>(get16(bytes, kOffResult));
        if (result == ResultCode::Success && internal_port && get16(bytes, 8) != *internal_port) {
            return false;
        }
        last_result_code_ = result;
        last_epoch_sec_ = get32(bytes, kOffEpoch);
        return true;
    };
    auto reply = udp_exchange::exchange(gateway_, port_, std::nullopt, request, kWaits, accept,
                                        "NAT-PMP", cancel_);
    if (!reply) return reply;
    const auto result = static_cast<ResultCode>(get16(reply.value, kOffResult));
    if (result != ResultCode::Success) {
        return fail<std::vector<uint8_t>>("NAT-PMP gateway answered result code " +
                                          std::to_string(static_cast<uint16_t>(result)) + " (" +
                                          result_code_name(result) + ")");
    }
    return reply;
}

}  // namespace lnsdk::natpmp

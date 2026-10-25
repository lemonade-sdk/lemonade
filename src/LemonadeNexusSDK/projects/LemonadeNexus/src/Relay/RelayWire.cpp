#include <LemonadeNexus/Relay/RelayWire.hpp>

#include <LemonadeNexus/Crypto/CryptoTypes.hpp>
#include <LemonadeNexus/Security/CanonicalEncoding.hpp>

#include <nlohmann/json.hpp>
#include <sodium.h>

#include <algorithm>
#include <cstring>
#include <exception>

namespace nexus::relay {

namespace {

void put_u16(uint8_t* out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value & 0xFF);
    out[1] = static_cast<uint8_t>(value >> 8);
}

void put_u32(uint8_t* out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
}

uint16_t get_u16(const uint8_t* in) {
    return static_cast<uint16_t>(in[0] | (in[1] << 8));
}

uint32_t get_u32(const uint8_t* in) {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) value |= static_cast<uint32_t>(in[i]) << (8 * i);
    return value;
}

/// HMAC-SHA256(shared_secret, canonical context), truncated to 16 bytes.
std::array<uint8_t, 16> derive_part(std::span<const uint8_t, 32> shared_secret,
                                    std::string_view label,
                                    const std::array<uint8_t, 32>& lo,
                                    const std::array<uint8_t, 32>& hi,
                                    std::string_view relay_id, uint64_t window) {
    security::CanonicalEncoder context(kRelayRendezvousDomain);
    context.add_string(label);
    context.add_string(relay_id);
    context.add_bytes(lo);
    context.add_bytes(hi);
    context.add_u64(window);
    const auto& message = context.bytes();

    std::array<uint8_t, crypto_auth_hmacsha256_BYTES> mac{};
    crypto_auth_hmacsha256(mac.data(), message.data(), message.size(), shared_secret.data());
    std::array<uint8_t, 16> out{};
    std::copy_n(mac.begin(), out.size(), out.begin());
    sodium_memzero(mac.data(), mac.size());
    return out;
}

/// Decode `text` (base64) into exactly N bytes.
template <std::size_t N>
bool decode_fixed_b64(const nlohmann::json& doc, const char* field, std::array<uint8_t, N>& out) {
    const auto it = doc.find(field);
    if (it == doc.end() || !it->is_string()) return false;
    try {
        const auto bytes = crypto::from_base64(it->get_ref<const std::string&>());
        if (bytes.size() != N) return false;
        std::copy(bytes.begin(), bytes.end(), out.begin());
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool read_id(const nlohmann::json& doc, const char* field, std::string& out) {
    const auto it = doc.find(field);
    if (it == doc.end() || !it->is_string()) return false;
    const auto& value = it->get_ref<const std::string&>();
    if (value.empty() || value.size() > kRelayMaxIdBytes) return false;
    out = value;
    return true;
}

bool read_u64(const nlohmann::json& doc, const char* field, uint64_t& out) {
    const auto it = doc.find(field);
    if (it == doc.end() || !it->is_number_unsigned()) return false;
    out = it->get<uint64_t>();
    return true;
}

}  // namespace

RelayCommitment relay_proof_commitment(const RelayProof& proof) {
    security::CanonicalEncoder encoder(kRelayProofCommitDomain);
    encoder.add_bytes(proof);
    return encoder.digest();
}

std::optional<RelayRendezvous> derive_relay_rendezvous(std::span<const uint8_t, 32> shared_secret,
                                                       const std::array<uint8_t, 32>& own_pk,
                                                       const std::array<uint8_t, 32>& peer_pk,
                                                       std::string_view relay_id,
                                                       uint64_t window) {
    if (relay_id.empty() || own_pk == peer_pk) return std::nullopt;
    if (sodium_is_zero(shared_secret.data(), shared_secret.size()) == 1) return std::nullopt;

    const bool own_is_low = std::memcmp(own_pk.data(), peer_pk.data(), own_pk.size()) < 0;
    const auto& lo = own_is_low ? own_pk : peer_pk;
    const auto& hi = own_is_low ? peer_pk : own_pk;

    RelayRendezvous out;
    out.id = derive_part(shared_secret, "id", lo, hi, relay_id, window);
    auto proof_lo = derive_part(shared_secret, "proof-low", lo, hi, relay_id, window);
    auto proof_hi = derive_part(shared_secret, "proof-high", lo, hi, relay_id, window);
    out.own_proof = own_is_low ? proof_lo : proof_hi;
    out.peer_commitment = relay_proof_commitment(own_is_low ? proof_hi : proof_lo);
    sodium_memzero(proof_lo.data(), proof_lo.size());
    sodium_memzero(proof_hi.data(), proof_hi.size());
    return out;
}

std::string relay_ticket_to_json(const RelayTicket& ticket) {
    nlohmann::json j{
        {"peer_id", ticket.peer_id},
        {"relay_id", ticket.relay_id},
        {"session_nonce", crypto::to_base64(std::span<const uint8_t>(ticket.session_nonce))},
        {"issued_at", ticket.issued_at},
        {"expires_at", ticket.expires_at},
        {"network_id", crypto::to_hex(std::span<const uint8_t>(ticket.network_id))},
        {"epoch", ticket.epoch},
        {"issuer", crypto::to_base64(std::span<const uint8_t>(ticket.issuer))},
        {"signature", crypto::to_base64(std::span<const uint8_t>(ticket.signature))},
    };
    return j.dump();
}

std::optional<RelayTicket> relay_ticket_from_json(std::string_view json_text) {
    if (json_text.empty() || json_text.size() > kRelayMaxTicketJsonBytes) return std::nullopt;
    const auto doc = nlohmann::json::parse(json_text, nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) return std::nullopt;

    RelayTicket ticket;
    if (!read_id(doc, "peer_id", ticket.peer_id) || !read_id(doc, "relay_id", ticket.relay_id) ||
        !read_u64(doc, "issued_at", ticket.issued_at) ||
        !read_u64(doc, "expires_at", ticket.expires_at) || !read_u64(doc, "epoch", ticket.epoch) ||
        !decode_fixed_b64(doc, "session_nonce", ticket.session_nonce) ||
        !decode_fixed_b64(doc, "issuer", ticket.issuer) ||
        !decode_fixed_b64(doc, "signature", ticket.signature)) {
        return std::nullopt;
    }
    const auto network = doc.find("network_id");
    if (network == doc.end() || !network->is_string()) return std::nullopt;
    const auto& network_hex = network->get_ref<const std::string&>();
    if (network_hex.size() != ticket.network_id.size() * 2) return std::nullopt;
    try {
        const auto bytes = crypto::from_hex(network_hex);
        if (bytes.size() != ticket.network_id.size()) return std::nullopt;
        std::copy(bytes.begin(), bytes.end(), ticket.network_id.begin());
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return ticket;
}

void encode_relay_header(const RelayPacketHeader& header, std::span<uint8_t> out) {
    if (out.size() < kRelayHeaderSize) return;
    uint8_t* p = out.data();
    put_u16(p, header.magic);
    p[2] = header.version;
    p[3] = static_cast<uint8_t>(header.msg_type);
    std::memcpy(p + 4, header.session_id.data(), header.session_id.size());
    put_u32(p + 20, header.sequence_number);
    put_u16(p + 24, header.payload_length);
}

std::optional<RelayPacketHeader> decode_relay_header(std::span<const uint8_t> datagram) {
    if (datagram.size() < kRelayHeaderSize) return std::nullopt;
    const uint8_t* p = datagram.data();
    RelayPacketHeader header{};
    header.magic = get_u16(p);
    header.version = p[2];
    header.msg_type = static_cast<RelayMsgType>(p[3]);
    std::memcpy(header.session_id.data(), p + 4, header.session_id.size());
    header.sequence_number = get_u32(p + 20);
    header.payload_length = get_u16(p + 24);
    if (header.magic != kRelayMagic || header.version != kRelayVersion) return std::nullopt;
    if (datagram.size() - kRelayHeaderSize < header.payload_length) return std::nullopt;
    return header;
}

std::vector<uint8_t> make_relay_datagram(RelayMsgType type, const SessionId& session,
                                         uint32_t sequence, std::span<const uint8_t> payload) {
    if (payload.size() > 0xFFFF) return {};
    std::vector<uint8_t> out(kRelayHeaderSize + payload.size());
    RelayPacketHeader header{};
    header.magic = kRelayMagic;
    header.version = kRelayVersion;
    header.msg_type = type;
    header.session_id = session;
    header.sequence_number = sequence;
    header.payload_length = static_cast<uint16_t>(payload.size());
    encode_relay_header(header, out);
    if (!payload.empty()) {
        std::memcpy(out.data() + kRelayHeaderSize, payload.data(), payload.size());
    }
    return out;
}

std::vector<uint8_t> make_relay_allocate(const RelayRendezvous& rendezvous,
                                         std::string_view ticket_json, uint32_t sequence) {
    if (ticket_json.empty() || ticket_json.size() > kRelayMaxTicketJsonBytes) return {};
    std::vector<uint8_t> payload;
    payload.reserve(kRelayAllocateFixedBytes + ticket_json.size());
    payload.insert(payload.end(), rendezvous.id.begin(), rendezvous.id.end());
    payload.insert(payload.end(), rendezvous.own_proof.begin(), rendezvous.own_proof.end());
    payload.insert(payload.end(), rendezvous.peer_commitment.begin(),
                   rendezvous.peer_commitment.end());
    payload.insert(payload.end(), ticket_json.begin(), ticket_json.end());
    return make_relay_datagram(RelayMsgType::Allocate, SessionId{}, sequence, payload);
}

std::optional<RelayAllocateRequest> decode_relay_allocate(std::span<const uint8_t> payload) {
    if (payload.size() <= kRelayAllocateFixedBytes ||
        payload.size() > kRelayAllocateFixedBytes + kRelayMaxTicketJsonBytes) {
        return std::nullopt;
    }
    RelayAllocateRequest request;
    std::memcpy(request.rendezvous.data(), payload.data(), 16);
    std::memcpy(request.proof.data(), payload.data() + 16, 16);
    std::memcpy(request.peer_commitment.data(), payload.data() + 32, 32);
    const auto json = payload.subspan(kRelayAllocateFixedBytes);
    auto ticket = relay_ticket_from_json(
        std::string_view(reinterpret_cast<const char*>(json.data()), json.size()));
    if (!ticket) return std::nullopt;
    request.ticket = std::move(*ticket);
    return request;
}

std::array<uint8_t, 5> encode_relay_allocated(const RelayAllocatedReply& reply) {
    std::array<uint8_t, 5> out{};
    out[0] = static_cast<uint8_t>(reply.state);
    put_u32(out.data() + 1, reply.idle_timeout_sec);
    return out;
}

std::optional<RelayAllocatedReply> decode_relay_allocated(std::span<const uint8_t> payload) {
    if (payload.size() < 5) return std::nullopt;
    const auto state = static_cast<RelayAllocationState>(payload[0]);
    if (state != RelayAllocationState::Waiting && state != RelayAllocationState::Paired) {
        return std::nullopt;
    }
    return RelayAllocatedReply{state, get_u32(payload.data() + 1)};
}

}  // namespace nexus::relay

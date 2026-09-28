#pragma once

/// C header for BoringTun's FFI API (matches the Rust crate's exported symbols).
/// Verified against boringtun 0.6.0 src/ffi/mod.rs — keep in sync if the crate
/// version changes. Includes extra exports from crates/boringtun-ffi/src/lib.rs.
/// See: https://github.com/cloudflare/boringtun

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Opaque tunnel handle (a Mutex<Tunn> on the Rust side — every call locks
/// internally, so a single handle is safe to use from multiple threads).
typedef struct Tunn Tunn;

/// Result type returned by wireguard_* functions.
enum result_type {
    WIREGUARD_DONE       = 0,  ///< No operation required
    WRITE_TO_NETWORK     = 1,  ///< Send dst buffer over UDP to the peer
    WIREGUARD_ERROR      = 2,  ///< Error occurred
    WRITE_TO_TUNNEL_IPV4 = 4,  ///< dst buffer is a plaintext IPv4 packet
    WRITE_TO_TUNNEL_IPV6 = 6,  ///< dst buffer is a plaintext IPv6 packet
};

/// Result struct: operation + byte count.
struct wireguard_result {
    enum result_type op;
    size_t           size;
};

/// Tunnel statistics snapshot.
struct stats {
    int64_t time_since_last_handshake;  ///< Seconds, or -1 if no handshake yet
    size_t  tx_bytes;                   ///< Data bytes encapsulated
    size_t  rx_bytes;                   ///< Data bytes decapsulated
    float   estimated_loss;
    int32_t estimated_rtt;              ///< Milliseconds, or -1 if unknown
    uint8_t reserved[56];
};

/// Create a new WireGuard tunnel (one Noise session pair with ONE peer).
/// @param static_private       Base64-encoded Curve25519 private key (ours)
/// @param server_static_public Base64-encoded Curve25519 public key of the peer
/// @param preshared_key        Optional base64 preshared key (NULL for none)
/// @param keep_alive           Persistent keepalive interval in seconds (0 = off)
/// @param index                24-bit peer index; local session receiver indices
///                             are (index << 8) | counter, so the peer for an
///                             incoming packet is recoverable as receiver_idx >> 8.
/// @return Opaque tunnel handle, or NULL on error
Tunn* new_tunnel(const char* static_private,
                 const char* server_static_public,
                 const char* preshared_key,
                 uint16_t    keep_alive,
                 uint32_t    index);

/// Free a tunnel handle.
void tunnel_free(Tunn* tunnel);

/// Encrypt an IP packet for sending over the network.
/// @param src      Plaintext IP packet
/// @param src_size Size of src
/// @param dst      Output buffer for encrypted WireGuard packet
/// @param dst_size Capacity of dst (should be >= src_size + 148)
struct wireguard_result wireguard_write(Tunn* tunnel,
                                       const uint8_t* src, uint32_t src_size,
                                       uint8_t* dst, uint32_t dst_size);

/// Decrypt a received WireGuard packet.
/// After a result other than WIREGUARD_DONE, call again with src=NULL,
/// src_size=0 to drain queued follow-up work (handshake replies, queued data).
/// @param src      Encrypted WireGuard packet from UDP
/// @param src_size Size of src
/// @param dst      Output buffer for decrypted IP packet
/// @param dst_size Capacity of dst
struct wireguard_result wireguard_read(Tunn* tunnel,
                                      const uint8_t* src, uint32_t src_size,
                                      uint8_t* dst, uint32_t dst_size);

/// Periodic timer — call every ~100-250 ms.
/// Handles keepalives and handshake retries.
struct wireguard_result wireguard_tick(Tunn* tunnel,
                                      uint8_t* dst, uint32_t dst_size);

/// Force a new handshake with the peer. dst must be >= 148 bytes.
struct wireguard_result wireguard_force_handshake(Tunn* tunnel,
                                                  uint8_t* dst, uint32_t dst_size);

/// Get tunnel statistics (last handshake age, rx/tx byte counts).
struct stats wireguard_stats(Tunn* tunnel);

/// Identify the initiator of a WireGuard handshake-initiation packet (type 1)
/// without any per-peer state: one DH against our static key decrypts the
/// initiator's static public key. Exported by crates/boringtun-ffi.
/// @param server_static_private Base64 Curve25519 private key (ours)
/// @param server_static_public  Base64 Curve25519 public key (ours)
/// @param src                   The raw UDP payload (148 bytes for type 1)
/// @param src_size              Size of src
/// @param out_peer_static_public 32-byte output: initiator's raw public key
/// @return 0 on success, -1 if not a valid handshake initiation for us
int32_t wireguard_parse_handshake_anon(const char* server_static_private,
                                       const char* server_static_public,
                                       const uint8_t* src, uint32_t src_size,
                                       uint8_t* out_peer_static_public);

#ifdef __cplusplus
}
#endif

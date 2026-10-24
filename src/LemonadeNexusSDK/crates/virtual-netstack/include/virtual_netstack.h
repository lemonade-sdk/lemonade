#pragma once

/// C header for the in-process virtual netstack (crates/virtual-netstack).
///
/// Terminates TCP/UDP addressed to the server's own virtual IPs without any
/// kernel network interface. The boringtun dataplane feeds decrypted IP
/// packets in via ns_feed_inbound; packets the stack emits are handed back
/// through the output callback for re-encryption and sending.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Opaque netstack handle.
typedef struct NsHandle NsHandle;

/// Callback invoked with each outbound IP packet the stack emits.
/// `ctx` is the pointer passed to ns_create.
typedef void (*ns_output_fn)(void* ctx, const uint8_t* pkt, size_t len);

/// Create a netstack with the given MTU. Spawns a dedicated stack thread.
/// Returns NULL on failure. `ctx` must outlive the stack.
NsHandle* ns_create(uint32_t mtu, ns_output_fn output, void* ctx);

/// Stop the stack thread and free the handle. Safe to call with NULL.
void ns_destroy(NsHandle* handle);

/// Register a virtual local address the stack answers for, e.g. "10.64.0.1/10".
/// The prefix makes that whole plane on-link (no routes needed). Returns 0 on
/// success, -1 on error.
int ns_add_local_ip(NsHandle* handle, const char* cidr);

/// Feed a decrypted inbound IP packet into the stack. Thread-safe; never
/// blocks the caller.
void ns_feed_inbound(NsHandle* handle, const uint8_t* pkt, size_t len);

/// Add an ingress forward: virtual `vip:vport` -> `target`, where target is
/// "tcp:127.0.0.1:PORT" or "unix:/path". Returns 0 on success, -1 on error.
int ns_add_tcp_forward(NsHandle* handle, const char* vip, uint16_t vport,
                       const char* target);

/// Add an egress: bind a real loopback TCP listener and bridge each accepted
/// connection to a virtual TCP connection toward `dst_ip:dst_port`, sourced
/// from `src_ip` (one of our virtual addresses). Returns the bound loopback
/// port, or 0 on failure.
uint16_t ns_add_tcp_egress(NsHandle* handle, const char* dst_ip,
                           uint16_t dst_port, const char* src_ip);

/// Close the egress listeners to `dst_ip:dst_port` and abort the virtual TCP
/// connections already open to it. Returns 0, or -1 for a null handle, an
/// invalid address, or a stopped stack.
int ns_remove_tcp_egress(NsHandle* handle, const char* dst_ip, uint16_t dst_port);

/// Callback invoked on the stack thread with each datagram received on a
/// bound virtual UDP port. Addresses are IPv4 in host byte order
/// (10.0.0.1 == 0x0A000001). `data` is valid only for the duration of the
/// call; copy it to keep it. The callback must not call ns_destroy.
typedef void (*ns_udp_recv_fn)(void* ctx, uint32_t src_ip, uint16_t src_port,
                               uint32_t dst_ip, uint16_t dst_port,
                               const uint8_t* data, size_t len);

/// Bind virtual UDP `vip:vport` (one of our virtual addresses). Each datagram
/// addressed to it is passed to `recv` with `ctx`. `ctx` must outlive the
/// stack. Binding the same endpoint twice keeps the first binding. Returns 0
/// on success, -1 on error.
int ns_udp_bind(NsHandle* handle, const char* vip, uint16_t vport,
                ns_udp_recv_fn recv, void* ctx);

/// Largest UDP payload ns_udp_send accepts (65535 - 20 IPv4 - 8 UDP).
#define NS_MAX_UDP_PAYLOAD 65507

/// Send one datagram from a bound virtual endpoint `src_ip:src_port` to
/// `dst_ip:dst_port`. Thread-safe; never blocks. Like a kernel UDP socket,
/// a datagram larger than the MTU is sent as IPv4 fragments and reassembled
/// by the receiving stack; `len` may be up to NS_MAX_UDP_PAYLOAD. Any number
/// of datagrams may be sent in series. A datagram from an unbound endpoint,
/// or one that still finds the send queue full after a flush, is dropped.
/// Returns 0 when queued, -1 on a parameter error.
int ns_udp_send(NsHandle* handle, const char* src_ip, uint16_t src_port,
                const char* dst_ip, uint16_t dst_port,
                const uint8_t* data, size_t len);

#ifdef __cplusplus
}
#endif

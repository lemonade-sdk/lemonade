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

int ns_remove_tcp_egress(NsHandle* handle, const char* dst_ip, uint16_t dst_port);

#ifdef __cplusplus
}
#endif

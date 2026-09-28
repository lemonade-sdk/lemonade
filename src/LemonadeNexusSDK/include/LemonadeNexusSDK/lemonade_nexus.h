#ifndef LEMONADE_NEXUS_SDK_H
#define LEMONADE_NEXUS_SDK_H

/**
 * @file lemonade_nexus.h
 * @brief C API for the Lemonade Nexus Client SDK.
 *
 * Opaque-handle API suitable for FFI from Python, Go, Rust, Swift, etc.
 * All complex outputs are returned as JSON strings via char** out_json.
 * Use ln_free() to release any string returned by the API.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/* Shared FFI build is compiled with hidden visibility; mark the ln_* C API
 * default-visible; platform export maps limit the shared interface to ln_*. */
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC visibility push(default)
#endif

/* ------------------------------------------------------------------ */
/* Error codes                                                         */
/* ------------------------------------------------------------------ */

typedef enum {
    LN_OK             =  0,
    LN_ERR_NULL_ARG   = -1,
    LN_ERR_CONNECT    = -2,
    LN_ERR_AUTH       = -3,
    LN_ERR_NOT_FOUND  = -4,
    LN_ERR_REJECTED   = -5,
    LN_ERR_NO_IDENTITY = -6,
    LN_ERR_PARSE      = -7, ///< Malformed response: HTTP 2xx with a body that did not
    LN_ERR_UNSUPPORTED = -8, ///< Retired operation: no server endpoint exists; kept
                            ///< exported so existing bindings fail explicitly.
                            ///< deserialize (e.g. a node endpoint returned 200 with a
                            ///< body that is not a valid TreeNode). Distinct from
                            ///< LN_ERR_NOT_FOUND (404), LN_ERR_AUTH (401/403) and
                            ///< LN_ERR_CONNECT (no transport / network error).
    LN_ERR_INTERNAL   = -99
} ln_error_t;

/* ------------------------------------------------------------------ */
/* Opaque handles                                                      */
/* ------------------------------------------------------------------ */

typedef struct ln_client_s  ln_client_t;
typedef struct ln_identity_s ln_identity_t;

/* ------------------------------------------------------------------ */
/* Memory management                                                   */
/* ------------------------------------------------------------------ */

/** Free a string returned by any ln_* function. */
void ln_free(char* ptr);

/* ------------------------------------------------------------------ */
/* Client lifecycle                                                    */
/* ------------------------------------------------------------------ */

/** Create a client connecting to host:port (plaintext HTTP). */
ln_client_t* ln_create(const char* host, uint16_t port);

/** Create a client connecting to host:port (TLS). */
ln_client_t* ln_create_tls(const char* host, uint16_t port);

/** Destroy a client and release resources. */
void ln_destroy(ln_client_t* client);

/* ------------------------------------------------------------------ */
/* Identity management                                                 */
/* ------------------------------------------------------------------ */

/** Generate a new Ed25519 identity keypair. */
ln_identity_t* ln_identity_generate(void);

/** Load an identity from a JSON file. */
ln_identity_t* ln_identity_load(const char* path);

/** Save an identity to a JSON file. */
ln_error_t ln_identity_save(const ln_identity_t* identity, const char* path);

/** Get the public key string ("ed25519:<base64>"). Caller must ln_free(). */
char* ln_identity_pubkey(const ln_identity_t* identity);

/** Destroy an identity and release resources. */
void ln_identity_destroy(ln_identity_t* identity);

/** Attach an identity to a client for delta signing. */
ln_error_t ln_set_identity(ln_client_t* client, const ln_identity_t* identity);

/* ------------------------------------------------------------------ */
/* Health                                                              */
/* ------------------------------------------------------------------ */

/** GET /api/health. Returns JSON via out_json. Caller must ln_free(*out_json). */
ln_error_t ln_health(ln_client_t* client, char** out_json);

/* ------------------------------------------------------------------ */
/* Authentication                                                      */
/* ------------------------------------------------------------------ */

/** Authenticate with username/password. Returns JSON via out_json. */
ln_error_t ln_auth_password(ln_client_t* client,
                             const char* username,
                             const char* password,
                             char** out_json);

/** Authenticate with passkey/FIDO2 data (JSON string). Returns JSON via out_json. */
ln_error_t ln_auth_passkey(ln_client_t* client,
                            const char* passkey_json,
                            char** out_json);

/** Issue the WebAuthn assertion challenge bound to user_id (POST /api/auth/challenge).
 *  The assertion's clientDataJSON must carry the returned challenge. Returns
 *  {"challenge":"base64url"} via out_json. */
ln_error_t ln_auth_passkey_challenge(ln_client_t* client,
                                     const char* user_id,
                                     char** out_json);

/** Authenticate with a token-link token. Returns JSON via out_json. */
ln_error_t ln_auth_token(ln_client_t* client,
                          const char* token,
                          char** out_json);

/* ------------------------------------------------------------------ */
/* Tree operations                                                     */
/* ------------------------------------------------------------------ */

/** GET /api/tree/node/{id}. Returns JSON via out_json. */
ln_error_t ln_tree_get_node(ln_client_t* client,
                             const char* node_id,
                             char** out_json);

/** POST /api/tree/delta. delta_json is a JSON string of the delta. */
ln_error_t ln_tree_submit_delta(ln_client_t* client,
                                 const char* delta_json,
                                 char** out_json);

/** Create a child node under parent_id. SDK handles delta signing.
 *  node_type: "endpoint", "customer", "relay". Returns JSON via out_json. */
ln_error_t ln_create_child_node(ln_client_t* client,
                                 const char* parent_id,
                                 const char* node_type,
                                 char** out_json);

/** Update an existing node. updates_json is a JSON object with fields to change.
 *  SDK handles delta signing. Returns JSON via out_json. */
ln_error_t ln_update_node(ln_client_t* client,
                           const char* node_id,
                           const char* updates_json,
                           char** out_json);

/** Delete a node. SDK handles delta signing. Returns JSON via out_json. */
ln_error_t ln_delete_node(ln_client_t* client,
                           const char* node_id,
                           char** out_json);

/* ------------------------------------------------------------------ */
/* IPAM                                                                */
/* ------------------------------------------------------------------ */

/** POST /api/ipam/allocate. block_type: "tunnel", "private", "shared". */
ln_error_t ln_ipam_allocate(ln_client_t* client,
                             const char* node_id,
                             const char* block_type,
                             char** out_json);

/* ------------------------------------------------------------------ */
/* Relay                                                               */
/* ------------------------------------------------------------------ */

/** GET /api/relay/list. Returns JSON array via out_json. */
ln_error_t ln_relay_list(ln_client_t* client, char** out_json);

/** POST /api/relay/ticket. Returns JSON via out_json. */
ln_error_t ln_relay_ticket(ln_client_t* client,
                            const char* peer_id,
                            const char* relay_id,
                            char** out_json);

/** POST /api/relay/register. reg_json is a JSON string. */
ln_error_t ln_relay_register(ln_client_t* client,
                              const char* reg_json,
                              char** out_json);

/* ------------------------------------------------------------------ */
/* Certificates                                                        */
/* ------------------------------------------------------------------ */

/** GET /api/certs/{domain}. Returns JSON via out_json. */
ln_error_t ln_cert_status(ln_client_t* client,
                           const char* domain,
                           char** out_json);

/** POST /api/certs/issue — request a TLS cert for this client's hostname.
 *  Returns JSON with encrypted cert bundle. Caller must ln_free(*out_json). */
ln_error_t ln_cert_request(ln_client_t* client,
                            const char* hostname,
                            char** out_json);

/** Decrypt an issued certificate bundle. bundle_json is the JSON from ln_cert_request.
 *  Returns JSON with {domain, fullchain_pem, privkey_pem, expires_at}.
 *  Caller must ln_free(*out_json). */
ln_error_t ln_cert_decrypt(ln_client_t* client,
                            const char* bundle_json,
                            char** out_json);

/* ------------------------------------------------------------------ */
/* Tree: children                                                      */
/* ------------------------------------------------------------------ */

/** GET /api/tree/children/{parent_id}. Returns JSON array via out_json. */
ln_error_t ln_tree_get_children(ln_client_t* client,
                                 const char* parent_id,
                                 char** out_json);

/* ------------------------------------------------------------------ */
/* Passkey registration                                                */
/* ------------------------------------------------------------------ */

/** POST /api/auth/register — register a passkey credential. */
ln_error_t ln_register_passkey(ln_client_t* client,
                                const char* user_id,
                                const char* credential_id,
                                const char* public_key_x,
                                const char* public_key_y,
                                char** out_json);

/* ------------------------------------------------------------------ */
/* Group membership                                                    */
/* ------------------------------------------------------------------ */

/** Add a member to a group node's assignments. permissions_json is a JSON array of strings. */
ln_error_t ln_add_group_member(ln_client_t* client,
                                const char* node_id,
                                const char* pubkey,
                                const char* permissions_json,
                                char** out_json);

/** Remove a member from a group node's assignments by pubkey. */
ln_error_t ln_remove_group_member(ln_client_t* client,
                                   const char* node_id,
                                   const char* pubkey,
                                   char** out_json);

/** Get members (assignments) of a group node. Returns JSON array via out_json. */
ln_error_t ln_get_group_members(ln_client_t* client,
                                 const char* node_id,
                                 char** out_json);

/** Join an existing group: create endpoint child + allocate tunnel IP. */
ln_error_t ln_join_group(ln_client_t* client,
                          const char* parent_node_id,
                          char** out_json);

/* ------------------------------------------------------------------ */
/* High-level operations                                               */
/* ------------------------------------------------------------------ */

/** Authenticate + create node + allocate IP. Returns JSON via out_json. */
ln_error_t ln_join_network(ln_client_t* client,
                            const char* username,
                            const char* password,
                            char** out_json);

/** Submit delete_node delta for current node. */
ln_error_t ln_leave_network(ln_client_t* client, char** out_json);

/* ------------------------------------------------------------------ */
/* Latency-based auto-switching                                        */
/* ------------------------------------------------------------------ */

/** Enable automatic server switching based on latency.
 *  @param threshold_ms  RTT threshold to trigger switch evaluation (default 200.0)
 *  @param hysteresis    Fraction by which new server must be better (default 0.3 = 30%)
 *  @param cooldown_sec  Minimum seconds between switches (default 60)
 */
ln_error_t ln_enable_auto_switching(ln_client_t* client,
                                     double threshold_ms,
                                     double hysteresis,
                                     uint32_t cooldown_sec);

/** Disable automatic server switching. */
ln_error_t ln_disable_auto_switching(ln_client_t* client);

/** Get current latency to the active server in milliseconds.
 *  Returns 0.0 if auto-switching is not enabled. */
double ln_current_latency_ms(ln_client_t* client);

/** Get latency stats for all known servers. Returns JSON array via out_json.
 *  Each element: {host, port, smoothed_rtt_ms, reachable, consecutive_failures}.
 *  Caller must ln_free(*out_json). */
ln_error_t ln_server_latencies(ln_client_t* client, char** out_json);

/* ------------------------------------------------------------------ */
/* Mesh dataplane                                                      */
/* ------------------------------------------------------------------ */

/** Generate a Curve25519 keypair for the mesh dataplane.
 *  Returns JSON: {private_key: "base64", public_key: "base64"}.
 *  Caller must ln_free(). */
char* ln_generate_keypair(void);

/* ------------------------------------------------------------------ */
/* Mesh P2P networking                                                 */
/* ------------------------------------------------------------------ */

/** Enable mesh networking with default config.
 *  Requires node_id to be set (via ln_set_node_id or ln_join_network). */
ln_error_t ln_mesh_enable(ln_client_t* client);

/** Enable mesh networking with custom config (JSON string).
 *  Config JSON keys: peer_refresh_interval_sec, heartbeat_interval_sec,
 *  stun_refresh_interval_sec, prefer_direct, auto_connect. */
ln_error_t ln_mesh_enable_config(ln_client_t* client, const char* config_json);

/** Disable mesh networking and remove all mesh peers from the tunnel. */
ln_error_t ln_mesh_disable(ln_client_t* client);

/** Get mesh tunnel status as JSON.
 *  Returns: {is_up, tunnel_ip, peer_count, online_count,
 *  total_rx_bytes, total_tx_bytes, peers: [...]}.
 *  Caller must ln_free(*out_json). */
ln_error_t ln_mesh_status(ln_client_t* client, char** out_json);

/** Get current mesh peers as JSON array.
 *  Each peer: {node_id, hostname, mesh_pubkey, tunnel_ip, private_subnet,
 *  endpoint, relay_endpoint, is_online, last_handshake, rx_bytes, tx_bytes,
 *  latency_ms, keepalive}.
 *  Caller must ln_free(*out_json). */
ln_error_t ln_mesh_peers(ln_client_t* client, char** out_json);

/** Force an immediate mesh peer refresh (fetch from server + sync tunnel). */
ln_error_t ln_mesh_refresh(ln_client_t* client);

/* ------------------------------------------------------------------ */
/* Identity: seed-based creation                                       */
/* ------------------------------------------------------------------ */

/** Create an Ed25519 identity from a 32-byte seed.
 *  Useful when the seed is derived from PBKDF2(username, password). */
ln_identity_t* ln_identity_from_seed(const uint8_t* seed, uint32_t seed_len);

/** Derive a 32-byte Ed25519 seed from username + password via PBKDF2-SHA256.
 *  Returns base64-encoded seed string. Caller must ln_free().
 *  Uses 100,000 iterations with salt "lemonade-nexus:{username}". */
char* ln_derive_seed(const char* username, const char* password);

/* ------------------------------------------------------------------ */
/* Account group-key envelopes (zero-knowledge multi-device sharing)   */
/* ------------------------------------------------------------------ */

/** Generate a fresh 32-byte account group key (base64). Caller must ln_free().
 *  Returns NULL on failure. */
char* ln_group_key_generate(void);

/** Seal a group key to a recipient device's Ed25519 public key
 *  ("ed25519:<b64>" or raw base64). Returns JSON {ephemeral_pubkey, wrapped_key}
 *  (base64). Caller must ln_free(). Returns NULL on malformed input. */
char* ln_group_key_wrap(const char* recipient_ed25519_pubkey, const char* group_key_b64);

/** Open a group-key envelope (JSON {ephemeral_pubkey, wrapped_key}) with the
 *  given identity's private key. Returns the group key (base64). Caller must
 *  ln_free(). Returns NULL if the envelope is not addressed to us / is corrupt. */
char* ln_group_key_unwrap(const ln_identity_t* identity, const char* envelope_json);

/* ------------------------------------------------------------------ */
/* Ed25519 challenge-response authentication                           */
/* ------------------------------------------------------------------ */

/** Authenticate using the client's Ed25519 identity (challenge-response).
 *  Requires ln_set_identity() to have been called first.
 *  Returns JSON: {authenticated, user_id, session_token, error}. */
ln_error_t ln_auth_ed25519(ln_client_t* client, char** out_json);

/* ------------------------------------------------------------------ */
/* Stats & server listing                                              */
/* ------------------------------------------------------------------ */

/** GET /api/stats. Returns JSON: {service, peer_count, private_api_enabled}. */
ln_error_t ln_stats(ln_client_t* client, char** out_json);

/** GET /api/servers. Returns JSON array of server entries. */
ln_error_t ln_servers(ln_client_t* client, char** out_json);

/* ------------------------------------------------------------------ */
/* Retired endpoints (kept for ABI stability)                          */
/* ------------------------------------------------------------------ */

/** Retired: /api/trust/status no longer exists. Returns LN_ERR_UNSUPPORTED. */
ln_error_t ln_trust_status(ln_client_t* client, char** out_json);

/// Generic authenticated call to a private-API route over the mesh.
/// `method` is "GET" or "POST"; `body_json` is the request body ("" for GET).
/// On success `*out_json` receives the raw JSON response body (free with
/// ln_free). Reachable only once the mesh tunnel is up.
ln_error_t ln_private_api_call(ln_client_t* client, const char* method,
                               const char* path, const char* body_json,
                               char** out_json);

/** Retired: /api/trust/peer/{pubkey} no longer exists. Returns LN_ERR_UNSUPPORTED. */
ln_error_t ln_trust_peer(ln_client_t* client, const char* pubkey, char** out_json);

/* ------------------------------------------------------------------ */
/* DDNS status                                                         */
/* ------------------------------------------------------------------ */

/** GET /api/ddns/status. Returns JSON with DDNS credential status. */
ln_error_t ln_ddns_status(ln_client_t* client, char** out_json);

/* ------------------------------------------------------------------ */
/* Retired endpoints (kept for ABI stability)                          */
/* ------------------------------------------------------------------ */

/** Retired: /api/enrollment/status no longer exists. Returns LN_ERR_UNSUPPORTED. */
ln_error_t ln_enrollment_status(ln_client_t* client, char** out_json);

/** Retired: /api/governance/proposals no longer exists. Returns LN_ERR_UNSUPPORTED. */
ln_error_t ln_governance_proposals(ln_client_t* client, char** out_json);

/** Retired: /api/governance/propose no longer exists. Returns LN_ERR_UNSUPPORTED. */
ln_error_t ln_governance_propose(ln_client_t* client,
                                   uint8_t parameter,
                                   const char* new_value,
                                   const char* rationale,
                                   char** out_json);

/* ------------------------------------------------------------------ */
/* Attestation manifests                                               */
/* ------------------------------------------------------------------ */

/** GET /api/attestation/manifests. Returns JSON with manifest list. */
ln_error_t ln_attestation_manifests(ln_client_t* client, char** out_json);

/* ------------------------------------------------------------------ */
/* Routing layer (connect-by-identifier)                               */
/* ------------------------------------------------------------------ */

/** GET /api/routing/profile. Returns the authorized endpoint profile JSON.
 *  Caller must ln_free(*out_json). */
ln_error_t ln_routing_profile(ln_client_t* client, int page, int page_size,
                              char** out_json);

/** POST /api/routing/request. conn_nonce_b64 is a client-chosen 16-byte nonce
 *  (base64). Returns {connection_id,state} JSON. Caller must ln_free(*out_json). */
ln_error_t ln_routing_request(ln_client_t* client, const char* identifier,
                              const char* conn_nonce_b64, const char* client_mesh_pubkey,
                              char** out_json);

/** GET /api/routing/session/{id}. Returns session-state JSON. ln_free(*out_json). */
ln_error_t ln_routing_connection_status(ln_client_t* client, const char* connection_id,
                                        char** out_json);

/* ------------------------------------------------------------------ */
/* Session management                                                  */
/* ------------------------------------------------------------------ */

/** Set the session token for authenticated API calls. */
ln_error_t ln_set_session_token(ln_client_t* client, const char* token);

/** Get the current session token. Caller must ln_free(). Returns NULL if not set. */
char* ln_get_session_token(ln_client_t* client);

/** Set the node ID. */
ln_error_t ln_set_node_id(ln_client_t* client, const char* node_id);

/** Get the current node ID. Caller must ln_free(). Returns NULL if not set. */
char* ln_get_node_id(ln_client_t* client);

/** Set a single-use device link token to include in the next ln_join_network. */
ln_error_t ln_set_link_token(ln_client_t* client, const char* token);

/** Mint a device link token for adding another device to this account's group.
 *  Requires an established mesh session. Returns {link_token, group_node_id,
 *  expires_at} JSON. ln_free(*out_json). */
ln_error_t ln_link_token_create(ln_client_t* client, uint32_t ttl_sec, char** out_json);

/** Publish a local service to mesh peers: our mesh IP:vport bridges to `target`
 *  ("tcp:127.0.0.1:PORT"). Requires an active mesh; re-applied on re-join. */
ln_error_t ln_mesh_expose_service(ln_client_t* client, uint16_t vport, const char* target);

/** Open (or reuse) a loopback bridge to a mesh peer's dst_ip:dst_port.
 *  On success *out_port is the bound 127.0.0.1 port to dial. */
ln_error_t ln_mesh_open_egress(ln_client_t* client, const char* dst_ip,
                               uint16_t dst_port, uint16_t* out_port);

int ln_mesh_is_active(ln_client_t* client);
ln_error_t ln_mesh_close_egress(ln_client_t* client, const char* dst_ip, uint16_t dst_port);

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC visibility pop
#endif

#ifdef __cplusplus
}
#endif

#endif /* LEMONADE_NEXUS_SDK_H */

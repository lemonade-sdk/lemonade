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
 * default-visible so only it is exported (MSVC uses WINDOWS_EXPORT_ALL_SYMBOLS). */
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
    LN_ERR_PORT_IN_USE = -9, ///< Another socket already owns the requested loopback
                             ///< port. Returned by ln_mesh_port_start.
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

/** Configure device-hosted account images after ln_set_identity.
 * cache_root is an absolute UTF-8 directory on this device, scoped to its identity.
 * The client owns the image service and a copy of the identity until destruction.
 * Do not configure concurrently with other avatar calls. No mesh connection is
 * needed until get/put. No image bytes are uploaded to the Nexus API.
 */
ln_error_t ln_account_avatar_configure(ln_client_t* client, const ln_identity_t* identity, const char* cache_root);

/** Sign a v2 image reference containing only group, author, revision and SHA-256.
 * image is a normalized PNG data URL up to 128 KiB and 128x128 pixels, or "" to
 * remove. revision is 1..2^53-1. Membership and Read/Write are checked separately.
 * No network request. Caller owns *out_json on success and must ln_free it.
 * Failure sets *out_json to null. Identity must remain alive during this call.
 */
ln_error_t ln_account_avatar_sign(const ln_identity_t* identity, const char* group_id,
                                  const char* image, uint64_t revision, char** out_json);

/** Read current references/permissions over the private API, then obtain bytes
 * from the local cache or authorized devices over the encrypted mesh. Returns
 * {group_id,revision,can_write,image,pending}. pending=true means no copy was
 * available; it is not a removal. Requires configure, authentication and join.
 * Caller owns *out_json (success or refusal); release it with ln_free. Unsupported
 * reference protocols return LN_ERR_UNSUPPORTED; missing APIs LN_ERR_NOT_FOUND.
 */
ln_error_t ln_account_avatar_get(ln_client_t* client, char** out_json);

/** Cache image locally and publish only the matching signed reference. Empty
 * image is removal. Does not retry conflicting writes. Ownership/errors follow
 * avatar_get. Lost responses retain the staged local copy for later retrieval.
 */
ln_error_t ln_account_avatar_put(ln_client_t* client, const char* record_json, const char* image, char** out_json);

/** Attach an identity to a client for delta signing. */
ln_error_t ln_set_identity(ln_client_t* client, const ln_identity_t* identity);

/* ------------------------------------------------------------------ */
/* Health                                                              */
/* ------------------------------------------------------------------ */

/** GET /api/health. Returns JSON via out_json:
 *  {"status","service","dns_base_domain","rp_id","ok"} and "error" when not ok.
 *  "rp_id" is the WebAuthn relying-party id the network's passkeys are bound to
 *  (empty when the server did not report one); a passkey client must use it,
 *  not a constant. Caller must ln_free(*out_json). */
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

/** POST /api/relay/ticket. Returns JSON via out_json: {peer_id, relay_id,
 *  session_nonce, issued_at, expires_at, network_id, epoch, issuer, signature}.
 *  A Tier 1 server signs the ticket; any relay verifies it against its
 *  verified Tier 1 membership. relay_id must be the relay's base64 node
 *  identity key. peer_id must be the caller's own node id. On failure the
 *  JSON carries "error" and the code reflects the HTTP status. */
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

/** The ln_cert_renew_if_needed() target that names this device's group
 *  loopback wildcard certificate, "*.<group-label>.lo.<base_domain>". */
#define LN_CERT_GROUP_LOOPBACK "*"

/** POST /api/certs/issue with scope "group_loopback" — request the wildcard
 *  certificate "*.<group-label>.lo.<base_domain>" of this device's group.
 *  Every name under it resolves to 127.0.0.1. Every device of the group gets
 *  the same private key. Requires an identity (ln_set_identity) and the
 *  session of a joined device of the group; other callers get LN_ERR_AUTH.
 *  Returns the same bundle JSON as ln_cert_request; decrypt it with
 *  ln_cert_decrypt. Caller must ln_free(*out_json). */
ln_error_t ln_cert_request_group(ln_client_t* client, char** out_json);

/** Called once after each certificate renewal, when both files are replaced.
 *  event_json: {target, domain, fullchain_path, privkey_path, expires_at}.
 *  event_json is valid only during the call; do not free it. Runs on an SDK
 *  background thread or on the thread that called ln_cert_renew_if_needed.
 *  The function must return normally (no longjmp, no C++ exception) and must
 *  not call ln_set_cert_renewed_callback or ln_destroy for this client. */
typedef void (*ln_cert_renewed_fn)(const char* event_json, void* user);

/** Set the renewal callback and its user pointer (fn NULL clears it). When
 *  this returns, the previous callback is not running and is not called
 *  again, so its user data may be released. No callback runs after
 *  ln_destroy returns. Returns LN_ERR_NULL_ARG when client is NULL. */
ln_error_t ln_set_cert_renewed_callback(ln_client_t* client,
                                        ln_cert_renewed_fn fn,
                                        void* user);

/** Keep a certificate in cert_dir current. hostname is the node hostname of
 *  the device certificate, or LN_CERT_GROUP_LOOPBACK. When
 *  cert_dir/fullchain.pem is missing or less than one third of its lifetime
 *  remains, requests the certificate (ln_cert_request flow), decrypts it,
 *  checks that the key matches, and replaces cert_dir/privkey.pem (mode 0600)
 *  and cert_dir/fullchain.pem through temporary files and rename. Then the
 *  renewal callback runs. On failure the old files stay. Registers the
 *  certificate for background renewal while the mesh is enabled; background
 *  attempts back off from 60 s to 6 h after failures. Blocks on the network
 *  when a request is due.
 *  out_json: {status: "renewed"|"not_due"|"failed", domain, expires_at,
 *  fullchain_path, privkey_path, error?}. Returns LN_OK for "renewed" and
 *  "not_due"; otherwise an error code (LN_ERR_AUTH, LN_ERR_CONNECT, ...).
 *  Caller must ln_free(*out_json). */
ln_error_t ln_cert_renew_if_needed(ln_client_t* client,
                                   const char* hostname,
                                   const char* cert_dir,
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

/** Set a member's permissions on a node, replacing its entry. permissions_json
 *  is a JSON array of permission names. The caller must hold "admin" on the node
 *  or an ancestor, and pubkey must belong to a device that joined inside that
 *  admin's group; the server refuses anything else. */
ln_error_t ln_add_group_member(ln_client_t* client,
                                const char* node_id,
                                const char* pubkey,
                                const char* permissions_json,
                                char** out_json);

/** Remove a member's entry from a node. Same rules as ln_add_group_member. */
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
 *  stun_refresh_interval_sec, prefer_direct, auto_connect.
 *  stun_refresh_interval_sec (default 60) sets how often the client learns
 *  its reflexive address from the server's STUN service and publishes it for
 *  hole punching; 0 turns this off. prefer_direct (default true) keeps
 *  probing a relayed peer's direct endpoint and moves the peer back to a
 *  direct path when a probe gets an authenticated handshake; false keeps
 *  every peer on a relay when one is available. */
ln_error_t ln_mesh_enable_config(ln_client_t* client, const char* config_json);

/** Disable mesh networking and remove all mesh peers from the tunnel. */
ln_error_t ln_mesh_disable(ln_client_t* client);

/** Get mesh tunnel status as JSON.
 *  Returns: {is_up, tunnel_ip, peer_count, online_count,
 *  total_rx_bytes, total_tx_bytes, peers: [...], needs_rejoin}.
 *  needs_rejoin is true when the Tier 1 server that issued the session left
 *  the verified membership and the SDK could not move the session by itself
 *  (no identity set, or the silent join failed; see
 *  ln_set_session_moved_callback): the session is dropped and private-API
 *  calls are refused until ln_join_network succeeds again, which clears it.
 *  Caller must ln_free(*out_json). */
ln_error_t ln_mesh_status(ln_client_t* client, char** out_json);

/** Called once after each session move. The Tier 1 server that issued the
 *  session leaves the membership at an epoch boundary: the SDK verifies the
 *  membership again from the pinned Genesis key and, with an identity set
 *  (ln_set_identity), authenticates through another verified Tier 1 server,
 *  joins again, and restarts the mesh as it was. The server's notice never
 *  chooses the next server. Without an identity, or when that join fails,
 *  the session is dropped and ln_mesh_status reports needs_rejoin.
 *  event_json: {from_node_id, from_host, to_node_id, to_host, epoch, silent,
 *  error}. silent is true when the SDK moved the session by itself (the
 *  application's state is intact); false when the application must call
 *  ln_join_network, and error says why. to_node_id and to_host are empty
 *  when no new session was issued.
 *  event_json is allocated for this call: the callback owns it and releases
 *  it with ln_free(), during the call or later from any thread, so a binding
 *  may hand the event to another thread. Runs on an SDK background thread,
 *  or on the thread whose call found the server gone. The function must
 *  return normally (no longjmp, no C++ exception) and must not call
 *  ln_set_session_moved_callback or ln_destroy for this client. */
typedef void (*ln_session_moved_fn)(char* event_json, void* user);

/** Set the session-move callback and its user pointer (fn NULL clears it).
 *  When this returns, the previous callback is not running and is not called
 *  again, so its user data may be released. No callback runs after
 *  ln_destroy returns. Returns LN_ERR_NULL_ARG when client is NULL. */
ln_error_t ln_set_session_moved_callback(ln_client_t* client,
                                         ln_session_moved_fn fn,
                                         void* user);

/** Get current mesh peers as JSON array.
 *  Each peer: {node_id, hostname, mesh_pubkey, tunnel_ip, private_subnet,
 *  endpoint, relay_endpoint, is_online, last_handshake, rx_bytes, tx_bytes,
 *  latency_ms, keepalive, last_seen, mgmt_pubkey, peer_permissions}.
 *  peer_permissions lists what the peer's key holds on this device and its
 *  ancestor groups ("admin", "svc:<family>", ...), as the server reports it;
 *  the service ingress (ln_mesh_service_set_catalog) enforces them.
 *  A peer whose traffic goes through a relay (relay fallback) has a non-empty
 *  relay_endpoint ("ip:port" of the relay); ln_mesh_status also reports its
 *  endpoint as empty.
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
/* Per-user identity store (protected by the OS keystore)              */
/* ------------------------------------------------------------------ */

/* The store keeps one identity for the effective OS user. The 32-byte seed
 * is wrapped with a random key-encryption key (KEK) and the KEK lives in the
 * OS keystore: the login keychain on macOS ("keychain"), a user-scope DPAPI
 * blob on Windows ("dpapi"), the Secret Service through libsecret on Linux
 * ("libsecret"). The file `identity.v2.json` in the user directory holds the
 * public key, the wrapped seed and the KEK reference; it never holds the seed
 * or the KEK. When no keystore is usable the functions return
 * LN_ERR_UNSUPPORTED and write nothing; there is no plaintext fallback.
 * The ln_identity_user_* functions serialize against each other and are
 * thread-safe. No output of these functions carries secret material. */

/** The per-user identity directory as UTF-8:
 *  macOS `~/Library/Application Support/LemonadeNexus/identity`,
 *  Windows `%LOCALAPPDATA%\LemonadeNexus\identity`,
 *  Linux `${XDG_DATA_HOME:-~/.local/share}/lemonade-nexus/identity`.
 *  The home directory is that of the effective user (never SUDO_USER); HOME
 *  is consulted only when the account database has none. The directory is
 *  not created by this call. Returns NULL when it cannot be resolved.
 *  Caller must ln_free(). */
char* ln_identity_user_dir(void);

/** Report the store as JSON {present, backend, unlocked, public_key?, dir}.
 *  `backend` is "keychain" | "dpapi" | "libsecret" | "none" (none means no
 *  usable keystore in this process). `unlocked` is true when the stored
 *  identity loads right now; computing it reads the KEK from the keystore,
 *  which may ask the OS for access. `public_key` ("ed25519:<base64>") is
 *  present only when a file exists. Caller must ln_free(*out_json).
 *  @return LN_OK, or LN_ERR_NULL_ARG for a NULL argument. */
ln_error_t ln_identity_user_status(char** out_json);

/** Load the user's stored identity. Caller owns the handle and must
 *  ln_identity_destroy() it. `out_err` may be NULL; otherwise it receives
 *  LN_OK, LN_ERR_NOT_FOUND (nothing stored), LN_ERR_AUTH (keystore locked
 *  or access denied), LN_ERR_UNSUPPORTED (no usable keystore, or the file
 *  was wrapped by another keystore), LN_ERR_REJECTED (the file or the
 *  wrapped seed failed verification, or the keystore no longer has the
 *  KEK: remove and create again), or LN_ERR_INTERNAL.
 *  @return the identity, or NULL on failure. */
ln_identity_t* ln_identity_user_load(ln_error_t* out_err);

/** Create a new identity for the user and store it. Fails with
 *  LN_ERR_REJECTED when an identity file is present, LN_ERR_UNSUPPORTED
 *  when no keystore is usable (no file is written), LN_ERR_AUTH when the
 *  keystore refuses, LN_ERR_INTERNAL when the file cannot be written (the
 *  keystore item is removed again). `out_err` may be NULL. Caller owns the
 *  handle and must ln_identity_destroy() it.
 *  @return the identity, or NULL on failure. */
ln_identity_t* ln_identity_user_create(ln_error_t* out_err);

/** Store an existing identity for the user, for a one-time migration from
 *  an identity.json file. The handle stays owned by the caller.
 *  @return LN_OK; LN_ERR_NULL_ARG; LN_ERR_REJECTED when an identity file is
 *          present or the identity is not valid; LN_ERR_UNSUPPORTED,
 *          LN_ERR_AUTH and LN_ERR_INTERNAL as for ln_identity_user_create. */
ln_error_t ln_identity_user_import(const ln_identity_t* identity);

/** Remove the user's stored identity: the keystore item first, then the
 *  file. The file stays when the keystore refuses to remove the item, so
 *  the call can be repeated.
 *  @return LN_OK; LN_ERR_NOT_FOUND when nothing is stored; LN_ERR_AUTH or
 *          LN_ERR_INTERNAL when the keystore item or the file cannot be
 *          removed. */
ln_error_t ln_identity_user_remove(void);

/* ------------------------------------------------------------------ */
/* Identity hand-off (single-use sealed transfer between processes)    */
/* ------------------------------------------------------------------ */

/* The receiver (for example a daemon) begins a hand-off and publishes the
 * request; the sender (a process in the user's session) seals its identity
 * to the request and returns the envelope; the receiver opens it once. The
 * ephemeral secret of a request is wiped on the first open whatever its
 * result, so an envelope cannot be replayed. A request expires after 60 s.
 * The envelope carries ciphertext only. A handle is not thread-safe except
 * for ln_identity_handoff_open, which serializes concurrent calls. */

typedef struct ln_handoff_s ln_handoff_t;

/** Receiver: create an ephemeral X25519 key and the request JSON
 *  {alg:"x25519-sealedbox-v1", pubkey, nonce, expires_at} (base64 values,
 *  expires_at in Unix seconds). Caller must ln_free(*out_request_json) and
 *  ln_identity_handoff_destroy() the handle.
 *  @return the handle, or NULL for a NULL argument or on failure
 *          (*out_request_json is then NULL). */
ln_handoff_t* ln_identity_handoff_begin(char** out_request_json);

/** Sender: seal `identity` to a request. Returns the envelope JSON
 *  {alg, nonce, sealed}; the seed is sealed together with the request nonce,
 *  so the envelope opens only for that request. Caller must ln_free().
 *  @return the envelope, or NULL for a NULL argument, an invalid identity,
 *          a malformed request, or an unknown alg. */
char* ln_identity_handoff_seal(const ln_identity_t* identity, const char* request_json);

/** Receiver: open an envelope. The first call on a handle consumes its
 *  ephemeral secret whatever the result; a later call fails. `out_err` may
 *  be NULL; otherwise it receives LN_OK, LN_ERR_NULL_ARG (nothing consumed),
 *  LN_ERR_REJECTED (handle already used, request expired, wrong alg, or an
 *  envelope for another request), LN_ERR_PARSE (malformed envelope),
 *  LN_ERR_AUTH (the sealed box does not open for this request), or
 *  LN_ERR_INTERNAL. Caller owns the returned handle and must
 *  ln_identity_destroy() it.
 *  @return the identity, or NULL on failure. */
ln_identity_t* ln_identity_handoff_open(ln_handoff_t* handoff, const char* envelope_json,
                                        ln_error_t* out_err);

/** Destroy a hand-off handle and wipe its ephemeral secret if still present.
 *  NULL is ignored. */
void ln_identity_handoff_destroy(ln_handoff_t* handoff);

/** Parse an identity from JSON text in the identity.json format
 *  {"public_key":"<base64>","private_key":"<base64>"} held in memory, for
 *  migration tooling. The caller keeps ownership of `json_text` and should
 *  wipe it afterwards. Caller must ln_identity_destroy() the handle.
 *  @return the identity, or NULL for a NULL argument or malformed text. */
ln_identity_t* ln_identity_from_json(const char* json_text);

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

/** Set the single-use invite to include in the next ln_join_network: the
 *  8-digit invite code or the full link token ("lnk_" and 64 hex digits),
 *  both from ln_invite_create on an already-joined device. "" clears it.
 *  LN_ERR_REJECTED, and nothing stored, for any other value; no request is
 *  sent. */
ln_error_t ln_set_link_token(ln_client_t* client, const char* token);

/** Join as an inference node (Tier 3, nonzero) or a client (Tier 4, zero, the
 *  default). Applies to the next ln_join_network; the server records it when it
 *  creates this device's endpoint. */
ln_error_t ln_set_inference_node(ln_client_t* client, int inference);

/** Mint a device link token for adding another device to this account's group.
 *  Same as ln_invite_create(client, NULL, NULL, ttl_sec, out_json).
 *  Requires an established mesh session. Returns {link_token, code,
 *  group_node_id, target_node_id, permissions, expires_at, code_expires_at}
 *  JSON. ln_free(*out_json). */
ln_error_t ln_link_token_create(ln_client_t* client, uint32_t ttl_sec, char** out_json);

/* ------------------------------------------------------------------ */
/* Invites, memberships, and moves                                     */
/* ------------------------------------------------------------------ */

/** Mint a single-use invite into a group: a link token and the 8-digit
 *  invite code that names the same invite. One call serves both invite
 *  flows:
 *   - target_node_id NULL or "": this account's own top-level group.
 *   - target_node_id a group id: that group (for example a subgroup, or
 *     another user's group where this account holds add_child).
 *  permissions_json: NULL for the server default (["read","connect_private"],
 *  reduced to what the caller holds), or a JSON array of permission names
 *  from "read", "write", "add_child", "connect_private", "connect_shared".
 *  The server refuses a target where the caller lacks add_child (403), a
 *  permission the caller does not hold (403), a name an invite cannot grant
 *  such as "admin" (400), and a target that is not a group (400).
 *  A new device joins under the target with the code or the token
 *  (ln_set_link_token and ln_join_network). An account that already joined
 *  becomes a member with ln_invite_accept. Requires an established mesh
 *  session.
 *  On success *out_json is {link_token, code, group_node_id, target_node_id,
 *  permissions, expires_at, code_expires_at}. link_token is "lnk_" and 64
 *  hex digits and works until expires_at. code is 8 decimal digits, works
 *  until code_expires_at (at most 10 minutes after minting). A server
 *  refuses code attempts from an address after 5 wrong codes within 10
 *  minutes, and all codes after 100 wrong codes from anywhere within 10
 *  minutes; the token still works then. Both are returned once and are
 *  never stored in clear.
 *  On failure *out_json carries "error" when the server answered. The
 *  strings passed in are not retained. The caller owns
 *  *out_json and must release it with ln_free(). Returns LN_ERR_NULL_ARG when
 *  client or out_json is NULL, LN_ERR_REJECTED for malformed
 *  permissions_json, else the mapped HTTP result. */
ln_error_t ln_invite_create(ln_client_t* client,
                            const char* target_node_id,
                            const char* permissions_json,
                            uint32_t ttl_sec,
                            char** out_json);

/** Accept an invite as an account that already joined ("join another
 *  network"). link_token is the 8-digit invite code or the full link token.
 *  This identity becomes a member of the invite's group with the invite's
 *  permissions; nothing this account owns moves. Its own device can then
 *  reach the group's devices, and they can reach it, as the permissions
 *  allow. The invite is single use and is not spent by a refusal.
 *  On success *out_json is {success, node_id, permissions, status}, where
 *  status is "member", or "pending" with an op_id while a tree operation
 *  commits. LN_ERR_REJECTED, with an "error" and no request sent, when
 *  link_token is neither a code nor a token; LN_ERR_AUTH for an invalid,
 *  expired, used, or locked-out invite or an inviter that lost its
 *  permission; LN_ERR_REJECTED when this identity already belongs to the
 *  group or has not joined. The caller owns *out_json and must release it
 *  with ln_free(). Requires an established mesh session. */
ln_error_t ln_invite_accept(ln_client_t* client,
                            const char* link_token,
                            char** out_json);

/** List the groups this identity belongs to. *out_json is {memberships:
 *  [{node_id, hostname, parent_id, owner_pubkey, role, permissions}]} where
 *  role is "home" (a group this identity joined or owns) or "member" (a
 *  group entered through an invite). permissions are this identity's own.
 *  The caller owns *out_json and must release it with ln_free(). Requires an
 *  established mesh session. */
ln_error_t ln_memberships_list(ln_client_t* client, char** out_json);

/** Leave a group entered through an invite: removes this identity's own
 *  entry on group_node_id. A group owner removes a member with
 *  ln_remove_group_member instead. Refused (LN_ERR_AUTH) for a home group.
 *  *out_json is {success, node_id, error?}. The caller owns *out_json and
 *  must release it with ln_free(). Requires an identity (ln_set_identity). */
ln_error_t ln_membership_leave(ln_client_t* client,
                               const char* group_node_id,
                               char** out_json);

/** Move a device (Endpoint) or a subgroup under another group. Needs
 *  delete_node on the device or admin above it (admin above a subgroup), and
 *  add_child on new_parent_id or admin above it. A top-level group and the
 *  root never move, and a node never moves under itself. The node keeps its
 *  id, tunnel address, and own assignments; the move grants nothing in the
 *  new group. *out_json is {success, node_id, parent_id}, or carries "error".
 *  LN_ERR_AUTH when the caller lacks a right, LN_ERR_NOT_FOUND for an unknown
 *  node, LN_ERR_REJECTED for an invalid move. The caller owns *out_json and
 *  must release it with ln_free(). Requires an established mesh session. */
ln_error_t ln_move_node(ln_client_t* client,
                        const char* node_id,
                        const char* new_parent_id,
                        char** out_json);

/** Check a device name before a join or a rename. Device names fold to one
 *  mesh-port label ("Work PC" and "work-pc" are both "work-pc"); the tree
 *  refuses a join or a hostname update whose label another device of the
 *  same group already holds (HTTP 409, an "error" that names that device),
 *  and a name with no letter or digit. This call asks a Tier 1 server first.
 *  Give group_node_id (this identity needs read on the group) or link_token
 *  (an invite, as its 8-digit code or full token: its target group is
 *  checked and the invite is NOT spent); the other may be NULL or "", but
 *  not both. Advisory only: a name reported available can still be taken
 *  before it is used.
 *  *out_json is {label, available, conflicts: [{node_id, device_name}],
 *  suggestion} (suggestion is a free label such as "work-pc-2"), or carries
 *  "error" when the server refused or was not reached. The caller owns
 *  *out_json and must release it with ln_free(). Needs a session (ln_auth_*
 *  or ln_set_session_token) and a verified Tier 1 server; no mesh session.
 *  LN_ERR_NULL_ARG when client, name, or out_json is NULL or neither scope is
 *  given, LN_ERR_REJECTED (no request sent) when link_token is neither a
 *  code nor a token, LN_ERR_AUTH without read on the group or for an invalid
 *  invite, LN_ERR_NOT_FOUND for an unknown group, LN_ERR_CONNECT when no
 *  Tier 1 server answered. */
ln_error_t ln_check_device_name(ln_client_t* client,
                                const char* name,
                                const char* group_node_id,
                                const char* link_token,
                                char** out_json);

/* ------------------------------------------------------------------ */
/* Account deletion                                                    */
/* ------------------------------------------------------------------ */

/** Permanently delete this identity's account. IRREVERSIBLE.
 *  The SDK asks the session's server for a challenge, signs an account
 *  deletion proof with the identity set by ln_set_identity (a session token
 *  alone cannot delete an account), and sends it. The server removes:
 *   - this account's groups (its "customer-<user_id>" group and every group
 *     or device its key owns) and everything beneath them, including devices
 *     that other users placed there through invites (those users keep their
 *     own accounts and lose access to this network);
 *   - this identity's memberships in other users' groups and every grant
 *     that names its keys;
 *   - its group-key envelopes, chat blobs, link tokens (invites), passkey
 *     credentials, device certificates, DNS names, and tunnel addresses;
 *   - its key: it can no longer authenticate, and its sessions end.
 *  The application root is never removed or changed. When this key also
 *  owns the root, the root and the key stay; only the account goes.
 *  On success (status "deleted") the client stops the mesh, clears its
 *  session token and node id, and stops the account image service. With tree
 *  operations on, the answer can be status "pending" with an op_id while
 *  Tier 1 commits the deletion.
 *  Local files stay. The caller deletes them after success: the identity
 *  file (ln_identity_save path), the account image cache
 *  (ln_account_avatar_configure cache_root), the certificate directory
 *  (ln_mesh_port_set_cert_dir), and any copy of the session token.
 *  On success *out_json is {success, status, account_id, removed_nodes?,
 *  op_id?}; on failure it carries "error". The caller owns *out_json and must
 *  release it with ln_free(). Returns LN_ERR_NULL_ARG for a NULL argument,
 *  LN_ERR_NO_IDENTITY without an identity, LN_ERR_AUTH for a refused proof or
 *  session, LN_ERR_CONNECT when the server cannot be reached, else the mapped
 *  HTTP result. Requires an established mesh session. Not thread-safe with
 *  other calls on the same client. */
ln_error_t ln_account_delete(ln_client_t* client, char** out_json);

/** Publish a local service to mesh peers: our mesh IP:vport bridges to `target`
 *  ("tcp:127.0.0.1:PORT"). Requires an active mesh; re-applied on re-join. A
 *  port with a service catalog (ln_mesh_service_set_catalog) is served through
 *  the SDK's ingress front end instead of a raw bridge; its target must be
 *  "tcp:<host>:<port>". */
ln_error_t ln_mesh_expose_service(ln_client_t* client, uint16_t vport, const char* target);

/* ------------------------------------------------------------------ */
/* Service catalog and route policy                                    */
/* ------------------------------------------------------------------ */

/** Register the route catalog of the service exposed on `vport`, replacing any
 *  previous catalog of that port. catalog_json:
 *    {"version": <uint>, "app": {"name": "...", "version": "..."},
 *     "families": [{"name": "inference",
 *                   "routes": [{"methods": ["POST"], "path": "/api/v1/chat/completions"},
 *                              {"methods": ["GET"], "prefix": "/api/v1/models"}]}]}
 *  `version` is required; `app` is optional. Family names match
 *  [a-z0-9][a-z0-9_.-]{0,62} and are unique. A route needs at least one method
 *  and exactly one of `path` (exact) or `prefix` (the prefix itself and every
 *  path below it), both absolute; the same method and path or prefix may
 *  appear only once. At most 64 families, 1024 routes and 256 KiB of JSON.
 *
 *  From then on the SDK terminates HTTP at that port's mesh ingress. For each
 *  request it attributes the connection to the mesh peer it came from (the
 *  source tunnel address, which the dataplane binds to the sending peer's
 *  key), matches method and path to the first route of the catalog, in
 *  catalog order, and forwards the request to the local service only when the
 *  peer's peer_permissions (ln_mesh_peers) hold "admin" or "svc:<family>".
 *  Allowed requests reach the service with the trusted headers
 *  X-Nexus-Peer-Node, X-Nexus-Peer-Key, X-Nexus-Peer-Permissions
 *  (comma-separated), X-Nexus-Family and X-Nexus-Catalog-Version; any
 *  client-supplied X-Nexus-* header is removed first. Status, Content-Type
 *  and streamed bodies pass through unchanged. GET /.nexus/catalog is
 *  answered by the SDK itself with the catalog for any attributed peer.
 *
 *  Every refusal answers HTTP 403 with Content-Type application/json and the
 *  body {"error", "code", "family", "required", "catalog_version",
 *  "app": {"name", "version"}}, where code is "nexus_permission_denied" (the
 *  peer lacks the family permission), "nexus_route_unregistered" (no route
 *  matches) or "nexus_peer_unattributed" (no listed peer accounts for the
 *  connection), and is reported to the ln_set_ingress_denied_callback
 *  callback. Ports without a catalog keep the raw bridge. Works before a
 *  join; an exposure already in force moves behind the front end at once.
 *  The string is copied.
 *  @return LN_OK; LN_ERR_NULL_ARG for a NULL argument; LN_ERR_REJECTED when
 *          the catalog is not valid JSON or breaks a rule above (the reason
 *          is logged); LN_ERR_CONNECT when the forward of a live exposure
 *          could not be installed. Thread-safe. */
ln_error_t ln_mesh_service_set_catalog(ln_client_t* client, uint16_t vport, const char* catalog_json);

/** The catalog registered for `vport`, as given, or "{}" when none. Caller
 *  must ln_free(*out_json). Returns LN_ERR_NULL_ARG for a NULL argument.
 *  Thread-safe. */
ln_error_t ln_mesh_service_catalog(ln_client_t* client, uint16_t vport, char** out_json);

/** Called for every request an ingress front end refuses.
 *  event_json: {kind ("denied" | "unregistered" | "unattributed"), vport,
 *  peer_node_id, peer_key, peer_tunnel_ip, method, path, family, required
 *  ("svc:<family>" or ""), catalog_version, time (Unix seconds)}. peer_key
 *  is the peer's management key when the peer list carries it, otherwise
 *  its mesh public key; peer_node_id and peer_key are empty for
 *  "unattributed". The event never carries a request body.
 *  event_json is allocated for this call: the callback owns it and releases
 *  it with ln_free(), during the call or later from any thread. Runs on an
 *  SDK thread while the refused request waits, so it must return promptly.
 *  The function must return normally (no longjmp, no C++ exception) and must
 *  not call ln_set_ingress_denied_callback or ln_destroy for this client. */
typedef void (*ln_ingress_denied_fn)(char* event_json, void* user);

/** Set the ingress refusal callback and its user pointer (fn NULL clears it).
 *  When this returns, the previous callback is not running and is not called
 *  again, so its user data may be released. No callback runs after
 *  ln_destroy returns. Returns LN_ERR_NULL_ARG when client is NULL. */
ln_error_t ln_set_ingress_denied_callback(ln_client_t* client,
                                          ln_ingress_denied_fn fn,
                                          void* user);

/** Open (or reuse) a loopback bridge to a mesh peer's dst_ip:dst_port.
 *  On success *out_port is the bound 127.0.0.1 port to dial. */
ln_error_t ln_mesh_open_egress(ln_client_t* client, const char* dst_ip,
                               uint16_t dst_port, uint16_t* out_port);

/** Close the loopback bridge to a mesh peer's dst_ip:dst_port and abort its
 *  open connections (for example after the peer is removed or re-addressed). */
ln_error_t ln_mesh_close_egress(ln_client_t* client, const char* dst_ip, uint16_t dst_port);

/** 1 when this client's mesh dataplane is up, 0 otherwise (or for NULL). */
int ln_mesh_is_active(ln_client_t* client);

/* ------------------------------------------------------------------ */
/* Tier 1 servers (tree traffic)                                       */
/* ------------------------------------------------------------------ */

/** Pin the network's Genesis public key and the lowest epoch to accept.
 *
 *  Only Tier 1 servers hold the permission tree. The client sends every tree
 *  read and write, every session request (ln_auth_*, ln_join_network), and
 *  every private-API call only to a Tier 1 server it verified from this key:
 *  it walks the authority chain from the key and keeps a member of the
 *  verified epoch only after the member signs an endpoint proof over a fresh
 *  nonce. Until a key is pinned those calls are not sent and fail with
 *  LN_ERR_CONNECT and an "error" that names the missing key.
 *
 *  @param genesis_pubkey_b64  The server config's `genesis_pubkey`: base64 of
 *                             32 bytes. Not retained after the call.
 *  @param min_epoch           The lowest Tier 1 epoch to accept (0 for none).
 *                             Store the epoch from ln_tier1_refresh and pass it
 *                             here after a restart to refuse an older list.
 *  @return LN_OK, LN_ERR_NULL_ARG, or LN_ERR_REJECTED for a malformed key.
 *  Changing the key drops the verified membership. Thread-safe. */
ln_error_t ln_set_genesis_key(ln_client_t* client, const char* genesis_pubkey_b64,
                              uint64_t min_epoch);

/** Verify the Tier 1 membership now from the client's server pool.
 *
 *  On LN_OK, *out_json receives
 *  {"epoch":N,"members":["<base64 node id>",...],
 *   "servers":[{"node_id":"<base64>","host":"<fqdn>","port":P},...]}.
 *  On failure it receives {"error":"..."} and the function returns
 *  LN_ERR_REJECTED (no pinned key, a chain that does not verify, or a stale
 *  epoch) or LN_ERR_CONNECT (no server reachable). The caller must ln_free()
 *  *out_json in both cases. Blocks on network I/O. Thread-safe. */
ln_error_t ln_tier1_refresh(ln_client_t* client, char** out_json);

/** The last verified Tier 1 membership, in the ln_tier1_refresh shape, with
 *  no network I/O. "servers" is empty when nothing is verified. The caller
 *  must ln_free() *out_json. Thread-safe. */
ln_error_t ln_tier1_membership(ln_client_t* client, char** out_json);

/** First-connect discovery of the network's Genesis key, for a client with no
 *  pinned key. It never replaces a pinned key; changing the key stays an
 *  explicit ln_set_genesis_key call.
 *
 *  The client asks each candidate server for GET /api/onboard/info over
 *  verified HTTPS: the servers in hosts_json first, then the configured
 *  server and the servers it lists in /api/servers. At most 16 distinct hosts
 *  are asked. Every server that reports a key must report the same key,
 *  network id, and DNS domain; the network id must derive from the key; and at
 *  least 3 distinct agreeing hosts must sit inside that DNS domain. Anything
 *  else fails closed and pins nothing.
 *
 *  Trust limit: this is trust on first use. It rests on verified-HTTPS
 *  ownership of the network's DNS domain and on agreement of several servers,
 *  not on a signature. The application must store the returned key and pin it
 *  with ln_set_genesis_key on later runs.
 *
 *  @param hosts_json  NULL, or a JSON array of extra candidate servers, for
 *                     example servers found through DNS discovery:
 *                     [{"host":"<certificate fqdn>","port":9100},...]. "port"
 *                     is optional (default 9100). Not retained after the call.
 *  @param out_json    Receives, on LN_OK,
 *                     {"genesis_pubkey":"<base64>","network_id":"<hex>",
 *                      "dns_base_domain":"<domain>","hosts":["<fqdn>",...]}
 *                     and on failure {"error":"..."}. The caller must ln_free()
 *                     *out_json in both cases. It stays NULL only for
 *                     LN_ERR_NULL_ARG and LN_ERR_INTERNAL.
 *  @return LN_OK and the key is pinned; LN_ERR_REJECTED when a key is already
 *          pinned, hosts_json is malformed, a server sent a malformed key or a
 *          network id that does not derive from its key, or servers disagree;
 *          LN_ERR_CONNECT when fewer than 3 agreeing servers answered.
 *  Blocks on network I/O. Thread-safe. */
ln_error_t ln_discover_genesis_key(ln_client_t* client, const char* hosts_json,
                                   char** out_json);

/** First-connect server discovery, the same steps as the client app's DNS
 *  discovery: resolve `<region>.seip.<domain>`, read each address's leaf
 *  certificate common name over TLS, and keep the names below `domain` (never
 *  the apex, never a wildcard). The handshake does not verify the certificate,
 *  so the names are candidates: create the client with ln_create_tls() on one
 *  of them, and let ln_discover_genesis_key() require agreement of several,
 *  before trusting anything they say.
 *  @param domain    Network DNS domain, e.g. "lemonade-nexus.io".
 *  @param region    SEIP region label, e.g. "us-west".
 *  @param port      HTTPS port to probe (9100 for the public API).
 *  @param out_json  Receives, on LN_OK,
 *                   {"servers":[{"host":"<fqdn>","port":9100,"address":"<ipv4>"},...]}
 *                   in resolution order with duplicate names removed, and on
 *                   failure {"error":"..."}. The caller must ln_free()
 *                   *out_json in both cases. It stays NULL only for
 *                   LN_ERR_NULL_ARG and LN_ERR_INTERNAL.
 *  @return LN_OK with at least one server; LN_ERR_REJECTED when domain or
 *          region is not a hostname label set; LN_ERR_CONNECT when the region
 *          name does not resolve or no address presented a usable name.
 *  Blocks on DNS and on one TLS handshake per address (at most 16, 5 s each).
 *  Thread-safe. */
ln_error_t ln_discover_servers(const char* domain, const char* region, uint16_t port,
                               char** out_json);

/* ------------------------------------------------------------------ */
/* Mesh port                                                           */
/* ------------------------------------------------------------------ */

/** Start the mesh port: a listener on 127.0.0.1:port that serves group devices
 *  by name. It never binds another address.
 *
 *  The listener serves plain HTTP until the group loopback certificate
 *  "*.<group-label>.lo.<base>" is available, then TLS. The certificate lives in
 *  the directory set with ln_mesh_port_set_cert_dir. On start, the port runs
 *  the ln_cert_renew_if_needed flow for LN_CERT_GROUP_LOOPBACK in that
 *  directory on a background thread. The SDK then renews the certificate in the
 *  background while the mesh is enabled, and the port reloads each renewed
 *  certificate in place for new connections. The port does not use a
 *  certificate that is not a group wildcard under ".lo.", that does not cover
 *  its own name, or whose key does not match. It then stays on plain HTTP, and
 *  ln_mesh_port_status reports the reason.
 *
 *  Routing uses the TLS server name and the Host header. They must name the
 *  same host, otherwise the answer is 421.
 *  - "<device>.<group>.lo.<base>" of another device is forwarded over mesh
 *    egress to that device's lemond on mesh port 13305. The device must be
 *    online and Tier 3 (otherwise 409), and this device must hold
 *    connect_private, connect_shared or admin on it (otherwise 403). The
 *    caller's own Authorization header is never forwarded to a peer. A request
 *    header "X-Lemonade-Peer-Authorization" is sent to the peer as
 *    Authorization. When that header is absent and the device is the selection
 *    target, the selection peer key is sent.
 *  - This device's own name, "127.0.0.1" and "localhost" are forwarded to the
 *    selection (see ln_mesh_port_set_selection).
 *  - An unknown device name answers 404. A name outside the group's local
 *    domain answers 421.
 *  Status codes, Content-Type and streamed bodies pass through unchanged. A
 *  request with an Origin header other than the requested origin answers 403.
 *
 *  Exactly one process owns the port. When another socket already owns
 *  127.0.0.1:port (for example lemond and the desktop app on the same machine),
 *  the call returns LN_ERR_PORT_IN_USE. It does not retry or take over the port.
 *
 *  @param client             The client. Its mesh session supplies the devices,
 *                            egress and certificates. Must not be NULL.
 *  @param port               Loopback port. 0 selects the default, 13306.
 *  @param local_service_url  The service that the "local" selection serves, for
 *                            example lemond's "http://127.0.0.1:13305", or NULL
 *                            for none. Only "http://127.0.0.1:<port>",
 *                            "http://localhost:<port>" and "http://[::1]:<port>"
 *                            are accepted. The string is copied.
 *  @return LN_OK when the port runs. When it already runs on the same port, only
 *          the local service URL changes. LN_ERR_NULL_ARG for a NULL client.
 *          LN_ERR_REJECTED for an invalid local service URL, or when the port
 *          already runs on a different port number. LN_ERR_PORT_IN_USE as
 *          described above. LN_ERR_INTERNAL when the listener cannot start.
 *  Thread-safe. ln_destroy stops the port. */
ln_error_t ln_mesh_port_start(ln_client_t* client, uint16_t port,
                              const char* local_service_url);

/** Set the directory of the mesh port's group loopback certificate, or NULL or
 *  "" for none. The directory holds fullchain.pem and privkey.pem (mode 0600),
 *  as written by ln_cert_renew_if_needed. Use a directory that only this user
 *  can read. Without a directory the port serves plain HTTP only. The setting
 *  stays across ln_mesh_port_stop and ln_mesh_port_start. When the port runs,
 *  a new directory is provisioned at once on the port's background thread. The
 *  string is copied. Returns LN_OK, or LN_ERR_NULL_ARG for a NULL client.
 *  Thread-safe. The mesh port receives renewal events in addition to the
 *  callback set with ln_set_cert_renewed_callback; that callback is unchanged. */
ln_error_t ln_mesh_port_set_cert_dir(ln_client_t* client, const char* cert_dir);

/** Stop the mesh port, abort its open requests, and clear the selection and its
 *  peer key. Blocks until the listener stops. Returns LN_OK, also when the port
 *  is not running, or LN_ERR_NULL_ARG for a NULL client. Thread-safe. */
ln_error_t ln_mesh_port_stop(ln_client_t* client);

/** Set the device that this device's own name and the loopback names serve.
 *
 *  @param client    The client. Must not be NULL.
 *  @param target    "local" (the local service given to ln_mesh_port_start),
 *                   "auto", a node ID, or NULL or "" to clear the selection.
 *                   This device's own node ID is the same as "local".
 *  @param peer_key  API key of the selected device's lemond, sent to it as
 *                   "Bearer <key>", or NULL for none. The key is kept only in
 *                   memory. Every call replaces it, so a new target clears it
 *                   unless the call passes a key again. The string is copied.
 *
 *  "auto" routes each request by its model. A "<device>/<model>" name goes to
 *  that device with the prefix removed. Then a model of the local service stays
 *  local. Then an idle online Tier 3 device with the model loaded serves it
 *  (lowest mesh latency first). Then a device that lists the model serves it.
 *  A GET of /api/v1/models (also /api/v0/models, /v1/models, /v0/models) returns
 *  the local models plus "<device>/<model>" entries.
 *
 *  Without a selection, or when the selected device is offline, requests answer
 *  409. The port never falls back to another device.
 *
 *  @return LN_OK. LN_ERR_NULL_ARG for a NULL client. LN_ERR_NOT_FOUND for an
 *          unknown device. LN_ERR_AUTH when this device may not connect to it.
 *          LN_ERR_REJECTED when the port is not running, "local" has no local
 *          service, or the device is not Tier 3. An offline Tier 3 device can be
 *          selected. Thread-safe. */
ln_error_t ln_mesh_port_set_selection(ln_client_t* client, const char* target,
                                      const char* peer_key);

/** Get the mesh port state as JSON:
 *  {running, port, scheme ("http" | "https"), local_domain, local_service,
 *   error, selection: {target (null | "local" | "auto" | node ID), peer_key_set},
 *   tls: {state ("off" | "waiting" | "activating" | "active" |
 *   "unsupported_certificate" | "error"), domain, expires_at, error},
 *   urls: [{node_id, device_name, url, self}],
 *   devices: [{node_id, device_name, is_online, app: {name, version},
 *              families: [...], catalog_status}],
 *   catalog: {...}}.
 *  "activating" means the first certificate is installed and the port switches
 *  to TLS as soon as the open plain HTTP requests finish; a running stream is
 *  never cut. `urls` lists this device and each online Tier 3 device that this
 *  device may connect to. Before TLS is active, only "http://127.0.0.1:<port>"
 *  is listed.
 *  `devices` lists the Tier 3 devices that can be selected, each with what its
 *  GET /.nexus/catalog last answered: the app name and version and the family
 *  names of its service catalog, and catalog_status (the HTTP status of that
 *  probe; 0 when the device has not answered yet, 404 when its SDK has no
 *  catalog). A device that reports no catalog shows empty strings. `catalog`
 *  is the catalog this client registered for mesh port 13305
 *  (ln_mesh_service_set_catalog), or {}. The peer key is never included.
 *  Caller must ln_free(*out_json). Returns LN_ERR_NULL_ARG for a NULL
 *  argument. Thread-safe. */
ln_error_t ln_mesh_port_status(ln_client_t* client, char** out_json);

#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC visibility pop
#endif

#ifdef __cplusplus
}
#endif

#endif /* LEMONADE_NEXUS_SDK_H */

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <httplib.h>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace lemon {
/// The listener a Nexus route is registered on. Public is lemond's normal port, where a
/// loopback client or a client on the local network (RFC 1918) gets every route and
/// `status.controller_url` names that same port;
/// Management is the ephemeral loopback listener the SDK session and the tray use;
/// MeshIngress is the loopback listener behind Nexus virtual port 13305, where the SDK
/// has already attributed the peer.
enum class NexusListener { Public, Management, MeshIngress };

/// Where the device identity lives.
/// System: `nexus/identity.json` under the data directory, generated on first use;
/// lemond is a system service without a signed-in user's keystore.
/// User: the SDK's per-user store (ln_identity_user_load/_create), protected by the OS
/// keystore of the user lemond runs as.
/// Handoff: lemond creates no identity. A process in the user's session seals the
/// user's stored identity to `GET key/handoff` and sends it with `POST key/unlock`.
enum class IdentitySource { System, User, Handoff };
/// The facts that decide the identity source: the platform, whether the effective user
/// is root, and whether systemd set STATE_DIRECTORY (only the system unit does).
struct ProcessRole {
    enum class Os { MacOS, Windows, Linux } os;
    bool root;
    bool state_directory;
};

namespace nexus_access {
extern const std::vector<std::string> kPermissions;
bool same_key(const std::string& a, const std::string& b);
bool grants(const nlohmann::json& permissions, const std::string& permission);
nlohmann::json account_summary(const nlohmann::json& members, const std::string& group_id,
    const std::string& node_id, const std::string& identity_id);
/// Validate bounded account avatar metadata against this session's group.
nlohmann::json account_avatar(const nlohmann::json& value, const std::string& group_id);
nlohmann::json tree_path(const std::string& id, const std::string& root_id,
    const std::function<nlohmann::json(const std::string&)>& fetch);
/// Same as tree_path, but the walk may end at any of `root_ids`: this device's group and
/// the groups this identity belongs to (ln_memberships_list). Throws 403 when no root is
/// an ancestor of `id`.
nlohmann::json tree_path(const std::string& id, const std::vector<std::string>& root_ids,
    const std::function<nlohmann::json(const std::string&)>& fetch);
/// Validates the permissions of an invite. Null selects the federation default and returns
/// null. Otherwise returns the ordered list. Throws 400 for a value that is not a list of
/// names, for "admin", and for a name an invite cannot grant; `svc:` names are allowed,
/// and the federation reduces them to what the inviter holds.
nlohmann::json invite_permissions(const nlohmann::json& requested);
/// True for an 8-digit invite code or a full "lnk_" invite token.
bool invite_code(const std::string& code);
/// The group entries an inviter completes after the federation placed the devices it
/// invited. The federation places a new device with `read` alone on the target group and
/// leaves the invite's permissions aside; the inviter holds them, so it applies them.
/// `group` is the target group node (`id`, `assignments`), `children` its tree children
/// (`type`, `id`, `mgmt_pubkey`), `invites` this device's outstanding invites for that
/// group ({target_node_id, permissions, expires_at}, in minting order) and `now` Unix
/// seconds. Each child endpoint other than `own_node_id` whose group entry is exactly
/// `read` takes the oldest unexpired invite: the result lists {management_pubkey,
/// permissions} with the invite's permissions plus `read`, and that invite and every
/// expired one are removed from `invites`. Children in id order, so the pairing is the
/// same on every call.
nlohmann::json pending_invite_grants(const nlohmann::json& group, const nlohmann::json& children,
    nlohmann::json& invites, const std::string& own_node_id, uint64_t now);
nlohmann::json tree_entry(const nlohmann::json& node, const std::string& self_key,
    bool ancestor_admin = false);
/// The device list lemond reports: the endpoints among `children` (this device's group),
/// then the entries of `network` (ln_list_network_devices) that are not children: devices
/// that joined this group from their own networks and devices of networks this identity
/// joined with an invite. Every entry carries `member` (joined its group from its own
/// network), `group_role` ("home" or "member"), and `can_connect` from `network`. A
/// `network` that is not an array means the list could not be fetched: the children then
/// carry `can_connect_known: false`, and `can_connect` is false rather than known.
nlohmann::json merge_members(const nlohmann::json& group, const nlohmann::json& children,
    const nlohmann::json& peers, const std::string& self_key, const std::string& self_node_id,
    const nlohmann::json& mesh_peers = nlohmann::json::array(),
    const nlohmann::json& network = nullptr);
nlohmann::json device_access(const nlohmann::json& members, const std::string& device_id,
    const std::string& self_node_id);
/// Route policy families of lemond's routes (RouteSpec::family), in catalog order.
/// The Nexus SDK enforces them at the mesh ingress with the `svc:<family>` permissions
/// of the calling peer; see docs/dev/nexus-integration.md, "Route policy".
extern const std::vector<std::string> kFamilies;
/// True for an opaque `svc:<family>` permission name: the family matches
/// [a-z0-9][a-z0-9_.-]{0,62}. lemond passes these names through to the tree.
bool service_permission(const std::string& name);
/// The group entry a role preset expands to: `administrator` (admin, connect_private
/// and every family), `moderator` (read, write, add_child, connect_private, svc:inference,
/// svc:mcp, svc:models_manage, svc:jobs) or `user` (read, connect_private, svc:inference).
/// `admin` and `member` are the former names of administrator and user. Throws 400 for
/// any other role.
nlohmann::json role_permissions(const std::string& role);
/// Validates a group entry: tree permission names and `svc:` names, `read` always
/// included, `admin` only through `admin`. Throws 400 otherwise.
nlohmann::json group_permissions(const nlohmann::json& requested, bool admin);
nlohmann::json device_permissions(const nlohmann::json& current, bool see, bool connect);
std::pair<int, std::string> assignment_error(const std::string& reason);
/// The verified Tier 1 epoch to save from an ln_tier1_membership reply, or 0 when the
/// reply carries no epoch above `saved`. The saved epoch only grows: it is the lowest
/// epoch the SDK accepts after a restart.
uint64_t newer_tier1_epoch(const nlohmann::json& membership, uint64_t saved);
/// True when the device must join again: the SDK dropped its session because the Tier 1
/// server that issued it left at an epoch boundary and the SDK could not move the
/// session itself. `mesh_status` is an ln_mesh_status reply; only its `needs_rejoin`
/// flag decides. The refusal texts are not a signal: the SDK answers the same texts
/// while it moves the session on its own, and a second join would race that move.
bool session_dropped(const nlohmann::json& mesh_status);
/// Decides the identity source from the process role, and nothing else:
/// Windows always User (LemonadeServer.exe runs as the signed-in user); macOS Handoff as
/// root (the LaunchDaemon cannot reach a login keychain) and User otherwise; Linux System
/// as root or under the system unit (STATE_DIRECTORY set), User otherwise (the per-user
/// unit or a terminal). No environment variable or option changes the result.
IdentitySource identity_source(const ProcessRole& role);
/// The role of the calling process.
ProcessRole current_process_role();
/// "system", "user" or "handoff", as `status` reports it.
const char* identity_source_name(IdentitySource source);
/// File-name-safe tag of an `ed25519:<base64>` public key: the first 16 characters of
/// its URL-safe base64 without padding, so the archived account of another identity is
/// `account-<tag>.json`. Empty for an empty key.
std::string account_file_tag(const std::string& pubkey);
/// The account that belongs to `pubkey` when another identity's account is active:
/// `archived` (that key's account, or null) with `server`, `genesis_pubkey`,
/// `tier1_epoch` and `device_name` carried from `previous` when it has none of its own.
/// Those are facts about the network and the machine, not about the identity; the
/// Genesis pin in particular must not drop with a key change. `public_key` is set.
nlohmann::json account_for_key(const nlohmann::json& previous, const nlohmann::json& archived, const std::string& pubkey);
/// How the active account relates to the key the federation just authenticated as
/// `user_id`. Own: the account has no user yet or the same user. Stale: another user,
/// and the account is not recorded as this key's (`public_key` absent, as files from
/// before it was recorded, or another key): archive it and start fresh; a delivered key
/// is never refused for a stale file. Conflict: the account is recorded as this key's
/// and the federation names another user: refuse.
enum class AccountMatch { Own, Stale, Conflict };
AccountMatch account_match(const nlohmann::json& account, const std::string& pubkey, const std::string& user_id);
/// The archive file name of a stale account: `account-<tag of its public_key>.json`
/// when the key is recorded, else `account-user-<tag of its user_id>.json`.
std::string stale_account_file(const nlohmann::json& account);
/// The permissions an invite grants when the request names neither a role nor a list:
/// the `user` preset (read, connect_private, svc:inference) reduced to what the inviter
/// holds on the target node (`held`; `admin` holds every name). The federation requires
/// the inviter to hold what it grants, so a name it lacks is dropped rather than failing
/// the invite. Returns {granted, dropped}; `granted` is null when nothing remains, which
/// selects the federation default.
std::pair<nlohmann::json, nlohmann::json> default_invite_permissions(const nlohmann::json& held, bool admin);
/// The HTTP answer for a failed ln_identity_handoff_open: 400 for a malformed envelope
/// (LN_ERR_PARSE, LN_ERR_NULL_ARG), 403 when the envelope does not open for the
/// outstanding request or the request was used or expired (LN_ERR_AUTH,
/// LN_ERR_REJECTED), 500 otherwise. The message never carries the envelope.
std::pair<int, std::string> handoff_failure(int code);
/// The HTTP answer for a failed SDK call. `retryable` is true when the federation was
/// not reached rather than refusing the device, so the same request may succeed later
/// and the local session stays as it is.
struct SdkFailure {
    int status;
    std::string message;
    bool retryable;
    // The network revoked this device's key (its account was deleted, or the device was
    // removed): the key never authenticates again, so the device needs a new one.
    bool identity_revoked{false};
};
/// Maps an SDK result to the answer lemond gives. `code` is the ln_error_t, `reply` the
/// SDK's {"error": ...} output and `rejected_status` the status for LN_ERR_REJECTED (the
/// federation refused the request as invalid for the current state). LN_ERR_CONNECT (no
/// transport, a timeout or a 5xx) and the SDK's messages for a federation it could not
/// reach answer 503 and are retryable. The SDK's refusal of a Genesis key discovery that
/// found too few agreeing servers (also reported as LN_ERR_CONNECT) answers 502, not
/// retryable, with the SDK text and the guidance to check DNS or set the key. Any other
/// LN_ERR_AUTH answers 403: the SDK reports a federation 401 and 403 alike.
/// `identity_revoked` carries the SDK's flag of the same name.
SdkFailure sdk_failure(int code, const nlohmann::json& reply, int rejected_status);
/// Compares two secrets in time that depends only on the length of `expected`.
bool constant_time_equals(const std::string& candidate, const std::string& expected);
/// Headless setup. A server whose identity is the system's (a Linux system unit) and that has
/// neither a passkey nor a first device-key unlock takes `key/unlock` from another machine only
/// with its setup code, which lemond writes to `setup-code` in its Nexus directory (the service
/// user and root can read it). Loopback callers, through SSH or on the server, need no code.
/// True for such a server's `status`.
bool setup_code_pending(const nlohmann::json& status);
/// A new setup code: 12 characters from an alphabet without look-alikes, as XXXX-XXXX-XXXX.
std::string new_setup_code();
/// Upper case without spaces or dashes, so a code typed or pasted in any form compares.
std::string normalize_setup_code(const std::string& code);
/// True when `authorization` is "Bearer <key>" for `api_key` or a nonempty `admin_key`.
bool bearer_matches(const std::string& authorization, const std::string& api_key, const std::string& admin_key);
/// Operations an authenticated mesh administrator may call.
bool remote_operation(const std::string& operation);
bool nexus_request(const httplib::Request& req);
bool administers_tree(const nlohmann::json& path, const std::string& key);
/// Only valid on the dedicated SDK ingress listener; public peer headers are untrusted.
std::pair<int, std::string> mesh_request_access(const httplib::Request& req, bool management = false);
/// {0, ""} runs the route, anything else is the HTTP error to answer (see NexusListener).
/// On the public listener a loopback or RFC 1918 client is a local operator; any other
/// client is refused. An API key never stands in for mesh caller authorization.
std::pair<int, std::string> route_access(NexusListener listener, const httplib::Request& req, const std::string& operation);
/// Header that carries the selected peer's API key through the relay. The relay sends it
/// to that peer as Authorization and never forwards the caller's own Authorization.
extern const char* const kPeerAuthorizationHeader;
/// Chooses the relay target among merged group members. Returns {0, tunnel address without
/// prefix length} or an HTTP error status and message: 400 for this device, 404 for a
/// device outside the group, 409 for a Tier 4 client, a device this device may not connect
/// to (`can_connect` false while `can_connect_known`), an offline device or one without a
/// mesh address.
std::pair<int, std::string> relay_target(const nlohmann::json& members, const std::string& self_node_id,
    const std::string& node_id);
/// The catalog revision: bump it when a route's family changes within one lemond
/// version. It must stay below 100.
extern const unsigned kCatalogRevision;
/// The mesh route catalog version: lemond's YEAR.WEEK.N version packed as
/// YEAR * 10^6 + WEEK * 10^3 + N, times 100, plus kCatalogRevision. It grows with every
/// release and with every revision bump, so the SDK and its refusal events can tell
/// catalogs apart. A version string without three leading numbers packs as 0.
uint64_t catalog_version(const std::string& lemond_version);
/// Builds the mesh route catalog the SDK enforces, from RouteRegistry::list():
/// `{"version", "app": {"name": "lemonade", "version"}, "families": [{"name", "routes":
/// [{"methods", "path" | "prefix"}]}]}`. A route path with a `{parameter}` becomes the
/// prefix before it; the other paths are exact, one per URL prefix. Throws
/// std::logic_error when a route has no valid family or two families claim one method
/// and URL, so a bad registration fails at startup.
nlohmann::ordered_json service_catalog(const nlohmann::ordered_json& routes, const std::string& lemond_version);
nlohmann::json route_permissions(const nlohmann::ordered_json& routes);
/// One log line for an SDK ingress refusal event (`{"kind", "method", "path",
/// "peer_node_id", "family", "required", "catalog_version", ...}`): "Nexus route policy:
/// <kind> <method> <path> from <peer_node_id> (family <family>, needs <required>, catalog
/// <version>)". Control characters are replaced and long fields are cut; no body or
/// secret is ever part of the event.
std::string ingress_denied_log_line(const nlohmann::json& event);
/// Headers the SDK sets on a request it admitted to the mesh ingress listener:
/// X-Nexus-Peer-Node, X-Nexus-Peer-Key, X-Nexus-Peer-Permissions, X-Nexus-Family and
/// X-Nexus-Catalog-Version. They are trusted only on NexusListener::MeshIngress.
extern const std::vector<std::string> kPeerHeaders;
/// The mesh peer attributed by the SDK, recorded for request logging.
struct PeerContext {
    std::string node_id;
    std::string family;
};
/// Runs once per request before any route. On the mesh ingress listener it records
/// the peer headers for request_peer(); on every other listener it removes them, so a
/// client cannot spoof a mesh peer, and clears request_peer().
void accept_peer_headers(httplib::Request& req, bool mesh_ingress);
/// The peer of the request the calling thread serves; empty outside a mesh ingress request.
const PeerContext& request_peer();
/// True while the calling thread serves a request a mesh peer sent. A peer uses this
/// server's models; it does not change how the operator loaded them (a load from a peer
/// keeps a loaded model as it is) or what is stored for them.
bool request_from_mesh_peer();
/// True when the loaded SDK exports the route catalog and refusal-event functions.
bool route_policy_available();
/// Sends a GET or POST `req` to http://127.0.0.1:`port``path` with lemond's HTTP client.
/// `peer_authorization` ("Bearer <key>" or empty) becomes the upstream Authorization;
/// `body` replaces the request body unless the request is multipart. A POST whose JSON
/// body sets "stream": true is streamed like a backend response: a peer that refuses it
/// before the first body byte keeps its status and body, and a stream that breaks after
/// that ends with one `event: error` SSE event. Other replies keep the peer's status and
/// body. A GET waits 30 s for the peer, a POST 300 s. An unreachable peer answers 502
/// with the transport failure in `detail`, and one WARNING line names `node_id`, the
/// method, the path without its query and that failure.
void relay_http(uint16_t port, const std::string& node_id, const std::string& path, const httplib::Request& req,
    httplib::Response& res, const std::string& peer_authorization, const std::string& body);
}

class NexusManager {
public:
    /// `install_policy` runs on the Management listener and `install_api` on MeshIngress,
    /// each once before it starts; NexusManager's authorization handlers are registered
    /// first, and `/internal/*` is never installed on MeshIngress. `mesh_port` 0 selects
    /// 13306. A null `service_catalog` registers no route policy with the SDK and status
    /// says so. `process_role` exists for tests, so none reaches an OS keystore; lemond
    /// leaves it unset.
    NexusManager(const std::string& config_dir,
                 const std::function<void(httplib::Server&)>& install_policy,
                 const std::function<void(httplib::Server&)>& install_api,
                 int mesh_port,
                 nlohmann::ordered_json service_catalog = nullptr,
                 std::optional<ProcessRole> process_role = std::nullopt,
                 const std::function<void(httplib::Server&)>& install_local_api = {},
                 nlohmann::json route_permissions = nullptr);
    ~NexusManager();
    void register_routes(httplib::Server& server, NexusListener listener = NexusListener::Public);
    void stop();
    nlohmann::json status() const;
    bool enable(std::string& error);
    void disable();
private:
    using json = nlohmann::json;
    struct Session;
    void supervise();
    void persist_enabled_locked();
    void update_snapshot_locked(const json& snapshot);
    void emit_locked(const std::string& type, const json& data);
    /// `controller_url`, when set, replaces the management listener in a status reply.
    void handle(const httplib::Request& req, httplib::Response& res, const std::string& operation, bool remote,
                const std::string& controller_url = {});
    void relay(const httplib::Request& req, httplib::Response& res);
    std::pair<int, json> control(const std::string& method, const std::string& path, const json& body,
                               const std::string& caller_key = "");
    // Headless setup (nexus_access::setup_code_pending), mutex_ held: the code in
    // <data_root_>/setup-code, created when first needed and removed once used.
    std::string setup_code_locked();
    void drop_setup_code_locked();
    std::string setup_code_file() const;
    int setup_code_failures_{0};
    std::string data_root_;
    nlohmann::ordered_json catalog_;
    const json route_permissions_;
    ProcessRole role_;
    int ingress_port_{0};
    std::unique_ptr<Session> session_;
    // The running session's SDK client (an ln_client_t*), so status() can report
    // the network key check's progress while a connect waits on it: session_mutex_
    // is held for the whole connect, and ln_tier1_progress never blocks. The
    // session clears it under `mutex` before it replaces or destroys the client.
    struct ClientSlot {
        std::mutex mutex;
        void* client = nullptr;
    };
    std::shared_ptr<ClientSlot> client_slot_ = std::make_shared<ClientSlot>();
    mutable std::mutex mutex_;
    // Serializes Session (SDK) work. Lock before mutex_ when both are needed.
    std::mutex session_mutex_;
    std::condition_variable wake_;
    bool enabled_{false};
    bool stopping_{false};
    json snapshot_ = {{"status", "disabled"}, {"locked", true}, {"mesh_up", false}, {"members", json::array()}};
    json active_invite_ = nullptr;
    uint64_t event_sequence_{0};
    std::deque<json> events_;
    std::thread worker_;
    httplib::Server management_server_;
    std::thread management_thread_;
    int management_port_{0};
    httplib::Server mesh_server_;
    std::thread mesh_thread_;
    int selection_port_{0};
};
}

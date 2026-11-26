#pragma once

#include <array>
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
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "lemon/recipe_options.h"

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
/// Tree permissions, named as the federation spells them. `kPermissions` holds every
/// value in the order lemond reports a permission list.
enum class Permission { Read, Write, AddChild, EditNode, DeleteNode, ExpandSubnet, ConnectPrivate, ConnectShared,
    RelayForward, RoutingCoordinate, StunRespond, RelayRegister, AllocateIp, Admin };
inline constexpr std::array<Permission, 14> kPermissions = {Permission::Read, Permission::Write, Permission::AddChild,
    Permission::EditNode, Permission::DeleteNode, Permission::ExpandSubnet, Permission::ConnectPrivate,
    Permission::ConnectShared, Permission::RelayForward, Permission::RoutingCoordinate, Permission::StunRespond,
    Permission::RelayRegister, Permission::AllocateIp, Permission::Admin};
std::string_view name(Permission permission);
std::optional<Permission> parse_permission(std::string_view text);
/// Route families of the lemond route catalog. The Nexus SDK enforces them at the mesh
/// ingress with the `svc:<family>` permissions of the calling peer; see
/// docs/dev/nexus-integration.md, "Route policy". `kFamilies` holds every value in the
/// order the administrator preset lists them.
enum class RouteFamily { Inference, Mcp, ModelsManage, Backends, Jobs, Internal, System, Management };
inline constexpr std::array<RouteFamily, 8> kFamilies = {RouteFamily::Inference, RouteFamily::Mcp,
    RouteFamily::ModelsManage, RouteFamily::Backends, RouteFamily::Jobs, RouteFamily::Internal, RouteFamily::System,
    RouteFamily::Management};
std::string_view name(RouteFamily family);
std::optional<RouteFamily> parse_family(std::string_view text);
/// The `svc:<family>` permission name of a family.
std::string service_permission(RouteFamily family);
/// Whether `name` is a well-formed `svc:` permission. The family only has to follow the
/// SDK grammar: the federation stores names lemond does not know.
bool service_permission(const std::string& name);
/// Member roles. `name` is the id reported in account and route-permission replies.
/// `parse_role` takes `administrator`, `moderator` and `user`, and the former names
/// `admin` and `member`.
enum class Role { Administrator, Moderator, User };
inline constexpr std::array<Role, 3> kRoles = {Role::Administrator, Role::Moderator, Role::User};
std::string_view name(Role role);
std::optional<Role> parse_role(std::string_view text);
nlohmann::json role_permissions(Role role);
/// The federation's node `type`. Other types exist on the tree (relays) and parse to
/// nothing; a tree entry reports the raw type as `node_type`.
enum class NodeType { Root, Customer, Endpoint };
std::string_view name(NodeType type);
std::optional<NodeType> parse_node_type(std::string_view text);
/// The `kind` of a tree entry: a group holds other nodes.
enum class TreeKind { Group, Node };
std::string_view name(TreeKind kind);
std::optional<TreeKind> parse_tree_kind(std::string_view text);
/// How the device unlocks: the account's `unlock` field and the status `unlock` value.
enum class UnlockMethod { Passkey, DeviceKey };
std::string_view name(UnlockMethod method);
std::optional<UnlockMethod> parse_unlock_method(std::string_view text);
/// A member's `connection_type`: this device, a direct path, the relay, or no active path.
enum class ConnectionType { Local, P2p, Relay, Unknown };
std::string_view name(ConnectionType type);
/// The account image `state`.
enum class AvatarState { Ready, Unsupported, Unavailable };
std::string_view name(AvatarState state);
std::optional<AvatarState> parse_avatar_state(std::string_view text);
/// The `status` value of a status reply.
enum class SessionStatus { Disabled, Locked, Ready, Joining, Running, Degraded };
std::string_view name(SessionStatus status);
/// Server-sent event names on `events`.
enum class EventType { LockChanged, MeshChanged, MemberJoined, MemberLeft };
std::string_view name(EventType type);
/// One value per Nexus route; `Relay` is the `peers/{node_id}/...` route. `name` is the
/// operation id, the route path without its `{node_id}/` segment.
enum class Operation { Status, PasskeyChallenge, KeyHandoff, Devices, Members, JoinStatus, Events, Tree, Account,
    Selection, Memberships, RoutePermissions, Enable, Disable, PasskeyRegister, PasskeyUnlock, KeyUnlock, Invites,
    InvitesCancel, Join, Egress, Lock, Device, DeviceCheck, AccountAvatar, InvitesAccept, AccountDelete, Forget,
    DevicesRemove, DevicesAccess, MembersRole, MembersPermissions, MembershipsLeave, TreeChildren, TreeGroups,
    TreeRename, TreePermissions, TreeInvites, TreeMove, Relay };
std::string_view name(Operation operation);
/// A route under `/v1/nexus/`; `path` may carry one `{node_id}` parameter. The table
/// order is the order of the route-permissions reply.
struct NexusRoute {
    const char* method;
    const char* path;
    Operation operation;
};
const std::vector<NexusRoute>& nexus_routes();
bool same_key(const std::string& a, const std::string& b);
/// Whether a federation permission list holds `permission` or `admin`. The string form
/// takes a `svc:` name.
bool grants(const nlohmann::json& permissions, Permission permission);
bool grants(const nlohmann::json& permissions, const std::string& permission);
nlohmann::json account_summary(const nlohmann::json& members, const std::string& group_id,
    const std::string& node_id, const std::string& identity_id);
nlohmann::json account_avatar(const nlohmann::json& value, const std::string& group_id);
nlohmann::json tree_path(const std::string& id, const std::string& root_id,
    const std::function<nlohmann::json(const std::string&)>& fetch);
nlohmann::json tree_path(const std::string& id, const std::vector<std::string>& root_ids,
    const std::function<nlohmann::json(const std::string&)>& fetch);
nlohmann::json invite_permissions(const nlohmann::json& requested);
bool invite_code(const std::string& code);
bool selection_key_allowed(const std::string& target, const std::string& peer_key);
/// The pin a load request asks for. A local operator's boolean `pinned` is returned as
/// sent (null when absent, so the router keeps a loaded model's pin). A mesh peer
/// never changes the host's pin: it gets null, and `pinned` leaves `options` and
/// `masked_saved_keys`, so a loaded model keeps its state and a fresh load takes the
/// host's saved default.
std::optional<bool> load_pin_request(bool mesh_peer, const nlohmann::json& pinned, RecipeOptions& options,
    std::set<std::string>& masked_saved_keys);
nlohmann::json tree_entry(const nlohmann::json& node, const std::string& self_key,
    bool ancestor_admin = false);
nlohmann::json merge_members(const nlohmann::json& group, const nlohmann::json& children,
    const nlohmann::json& peers, const std::string& self_key, const std::string& self_node_id,
    const nlohmann::json& mesh_peers = nlohmann::json::array(),
    const nlohmann::json& network = nullptr);
nlohmann::json device_access(const nlohmann::json& members, const std::string& device_id,
    const std::string& self_node_id);
nlohmann::json group_permissions(const nlohmann::json& requested, bool admin);
nlohmann::json device_permissions(const nlohmann::json& current, bool see, bool connect);
std::pair<int, std::string> assignment_error(const std::string& reason);
uint64_t newer_tier1_epoch(const nlohmann::json& membership, uint64_t saved);
bool session_dropped(const nlohmann::json& mesh_status);
/// Why the mesh transport is not up after `ln_mesh_enable` or `ln_mesh_enable_config`
/// and `ln_mesh_expose_service`, or "" when it is. `port` is the configured
/// `mesh_udp_port` (0 = any port). The SDK answers LN_OK and leaves the dataplane down
/// when a fixed port cannot be bound, so with a fixed port the `dataplane_port` in use
/// decides, and the text names the port.
std::string mesh_transport_failure(bool started, int port, const nlohmann::json& mesh_status);
/// Decides the identity source from the process role, and nothing else:
/// Windows always User (LemonadeServer.exe runs as the signed-in user); macOS Handoff as
/// root (the LaunchDaemon cannot reach a login keychain) and User otherwise; Linux System
/// as root or under the system unit (STATE_DIRECTORY set), User otherwise (the per-user
/// unit or a terminal). No environment variable or option changes the result.
IdentitySource identity_source(const ProcessRole& role);
ProcessRole current_process_role();
const char* identity_source_name(IdentitySource source);
std::string account_file_tag(const std::string& pubkey);
/// The account that belongs to `pubkey` when another identity's account is active:
/// `archived` (that key's account, or null) with `server`, `genesis_pubkey`,
/// `tier1_epoch`, `device_name` and `mesh_udp_port` carried from `previous` when it has
/// none of its own: facts about the network and the machine, not about the identity.
/// The Genesis pin in particular must not drop with a key change.
nlohmann::json account_for_key(const nlohmann::json& previous, const nlohmann::json& archived, const std::string& pubkey);
enum class AccountMatch { Own, Stale, Conflict };
AccountMatch account_match(const nlohmann::json& account, const std::string& pubkey, const std::string& user_id);
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
struct SdkFailure {
    int status;
    std::string message;
    bool retryable;
    bool identity_revoked{false};
};
/// LN_ERR_AUTH answers 403: the SDK reports a federation 401 and 403 alike.
SdkFailure sdk_failure(int code, const nlohmann::json& reply, int rejected_status);
/// Compares two secrets in time that depends only on the length of `expected`.
bool constant_time_equals(const std::string& candidate, const std::string& expected);
bool bearer_matches(const std::string& authorization, const std::string& api_key, const std::string& admin_key);
/// Whether a mesh administrator may run the operation remotely; the others concern the
/// local identity, lifecycle and relay.
bool remote_operation(Operation operation);
bool nexus_request(const httplib::Request& req);
bool administers_tree(const nlohmann::json& path, const std::string& key);
/// Only valid on the dedicated SDK ingress listener; public peer headers are untrusted.
std::pair<int, std::string> mesh_request_access(const httplib::Request& req, bool management = false);
/// {0, ""} runs the route, anything else is the HTTP error to answer (see NexusListener).
/// On the public listener a loopback or RFC 1918 client is a local operator; any other
/// client is refused. An API key never stands in for mesh caller authorization.
std::pair<int, std::string> route_access(NexusListener listener, const httplib::Request& req, Operation operation);
/// Header that carries the selected peer's API key through the relay. The relay sends it
/// to that peer as Authorization and never forwards the caller's own Authorization.
extern const char* const kPeerAuthorizationHeader;
std::pair<int, std::string> relay_target(const nlohmann::json& members, const std::string& self_node_id,
    const std::string& node_id);
bool relay_retryable(const std::string& reason);
struct RelayFailure {
    int status{502};
    std::string error;
    std::string detail;
};
RelayFailure relay_failure(int curl_code, const std::string& reason, long waited_seconds);
/// The catalog revision: bump it when a route's family changes within one lemond
/// version. It must stay below 100.
extern const unsigned kCatalogRevision;
uint64_t catalog_version(const std::string& lemond_version);
nlohmann::ordered_json service_catalog(const nlohmann::ordered_json& routes, const std::string& lemond_version);
nlohmann::json route_permissions(const nlohmann::ordered_json& routes);
std::string ingress_denied_log_line(const nlohmann::json& event);
extern const std::vector<std::string> kPeerHeaders;
/// The peer the SDK attributed; `family` is empty when the X-Nexus-Family header names
/// no family lemond knows.
struct PeerContext {
    std::string node_id;
    std::optional<RouteFamily> family;
};
void accept_peer_headers(httplib::Request& req, bool mesh_ingress);
/// The peer of the request the calling thread serves; empty outside a mesh ingress request.
const PeerContext& request_peer();
bool request_from_mesh_peer();
bool route_policy_available();
void relay_http(uint16_t port, const std::string& node_id, const std::string& path, const httplib::Request& req,
    httplib::Response& res, const std::string& peer_authorization, const std::string& body,
    const std::function<uint16_t()>& reopen = {});
}

class NexusManager {
public:
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
    void emit_locked(nexus_access::EventType type, const json& data);
    void handle(const httplib::Request& req, httplib::Response& res, nexus_access::Operation operation, bool remote,
                const std::string& controller_url = {});
    void relay(const httplib::Request& req, httplib::Response& res);
    std::pair<int, json> control(nexus_access::Operation operation, const std::string& method, const std::string& node_id,
                                 const json& body, const std::string& caller_key = "");
    std::string data_root_;
    nlohmann::ordered_json catalog_;
    const json route_permissions_;
    ProcessRole role_;
    int ingress_port_{0};
    std::unique_ptr<Session> session_;
    // The running session's SDK client (an ln_client_t*), read by status() while
    // session_mutex_ is held for a whole connect; ln_tier1_progress never blocks. The
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
    json snapshot_ = {{"status", nexus_access::name(nexus_access::SessionStatus::Disabled)}, {"locked", true},
        {"mesh_up", false}, {"members", json::array()}};
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

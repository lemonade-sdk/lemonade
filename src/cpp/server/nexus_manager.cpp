#include "lemon/nexus_manager.h"
#include "lemon/error_types.h"
#include "lemon/nexus_user_key.h"
#include "lemon/server/route_registry.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/network_beacon.h"
#include "lemon/utils/path_utils.h"
#include "lemon/version.h"
#include <lemon/utils/aixlog.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#endif
#include <LemonadeNexusSDK/lemonade_nexus.h>
#include <curl/curl.h>

namespace lemon {
namespace {
using json = nlohmann::json;
using namespace std::chrono_literals;
// The /api/v1 prefix of the older routes is legacy and new routes do not take it, so
// there is no alias.
constexpr const char* nexus_prefix = "/v1/nexus/";
constexpr const char* kLogTag = "Nexus";
constexpr const char* kSdkLogTag = "Nexus SDK";
constexpr const char* kJsonContentType = "application/json";
constexpr const char* kEventStreamContentType = "text/event-stream";
constexpr const char* kClientSessionHeader = "X-Client-Session-Id";
constexpr const char* kPeerNodeHeader = "X-Nexus-Peer-Node";
constexpr const char* kPeerKeyHeader = "X-Nexus-Peer-Key";
constexpr const char* kPeerPermissionsHeader = "X-Nexus-Peer-Permissions";
constexpr const char* kFamilyHeader = "X-Nexus-Family";
constexpr const char* kCatalogVersionHeader = "X-Nexus-Catalog-Version";
// Nexus virtual port behind which the SDK reaches lemond's API.
constexpr uint16_t kMeshServicePort = 13305;
constexpr int kDefaultServerPort = 9100;
constexpr int kMaxPort = 65535;
constexpr size_t kMaxNameBytes = 128;
constexpr size_t kMaxHierarchyDepth = 128;
constexpr size_t kMaxNodeIdLength = 256;
constexpr size_t kAccountTagLength = 16;
constexpr size_t kMaxRequestBodyBytes = 64 * 1024;
constexpr size_t kMaxAvatarImageBytes = 128 * 1024;
constexpr size_t kAvatarRequestEnvelopeBytes = 2048;
// The largest integer a JavaScript client reads exactly.
constexpr uint64_t kMaxSafeInteger = 9007199254740991ULL;
constexpr const char* kPngDataUrlPrefix = "data:image/png;base64,";
constexpr size_t kLinkTokenLength = 68;
constexpr const char* kLinkTokenPrefix = "lnk_";
constexpr size_t kInviteCodeDigits = 8;
constexpr int kInviteTtlMinSeconds = 60;
constexpr int kInviteTtlMaxSeconds = 600;
constexpr size_t kMaxQueuedEvents = 128;
constexpr std::chrono::seconds kRetryDelayInitial = 5s;
constexpr std::chrono::seconds kRetryDelayMax = 60s;
constexpr std::chrono::seconds kSessionDropRetryDelay = 1s;
constexpr std::chrono::seconds kSupervisorPeriod = 5s;
constexpr std::chrono::minutes kIdleLockAfter = 5min;
constexpr time_t kMeshListenerReadTimeoutSeconds = 30;
constexpr time_t kMeshListenerWriteTimeoutSeconds = 300;
constexpr long kRelayStreamTimeoutSeconds = 300;
// WebAuthn authenticator data: the flags byte follows the 32-byte RP ID hash, and
// user presence (0x01) and user verification (0x04) must both be set.
constexpr size_t kAuthenticatorDataMinBytes = 37;
constexpr size_t kAuthenticatorFlagsOffset = 32;
constexpr unsigned char kUserPresentAndVerified = 0x05;
constexpr std::string_view kEd25519Prefix = "ed25519:";
constexpr std::string_view kNodeIdParameter = "{node_id}";
constexpr const char* kTreeNodeApi = "/api/tree/node/";
constexpr const char* kTreeChildrenApi = "/api/tree/children/";
constexpr const char* kAccountFile = "account.json";
constexpr const char* kIdentityFile = "identity.json";
constexpr const char* kServicePermissionPrefix = "svc:";
constexpr const char* kMsgPermissionList = "Send permissions as a list of names";
constexpr const char* kMsgUnlockFirst = "Unlock your network first";
constexpr const char* kMsgJoinFirst = "Join the network first";
constexpr const char* kMsgNotInGroup = "That device is not in your group";
constexpr const char* kMsgChooseAnotherMember = "Choose another member of your group";
constexpr const char* kMsgInvalidData = "The network returned invalid data";
const std::string kMsgDeviceName = "Enter a device name (up to " + std::to_string(kMaxNameBytes) + " characters)";
const std::string kMsgName = "Enter a name of up to " + std::to_string(kMaxNameBytes) + " characters";
const std::string kMsgGroupName = "Enter a group name (up to " + std::to_string(kMaxNameBytes) + " UTF-8 bytes)";
const std::string kMsgInviteCode = "Enter an " + std::to_string(kInviteCodeDigits) + "-digit code or a full invite token";
constexpr const char* kMsgAccountChanged = "The account changed. Sign in again.";
constexpr const char* kMsgOwnAccess = "You cannot change your own access";
constexpr const char* kMsgCreatorAdministers = "The group creator always administers the group";
constexpr const char* kMsgOnlyGroupAdmin = "Only a group admin can change member access";
constexpr const char* kMsgUnknownNetwork = "Unknown network operation";
constexpr const char* kMsgAvatarNotSaved = "Profile image could not be saved";
constexpr const char* kMsgChooseRole = "Choose the administrator, moderator or user role";
using nexus_access::AvatarState;
using nexus_access::ConnectionType;
using nexus_access::EventType;
using nexus_access::NodeType;
using nexus_access::Operation;
using nexus_access::Permission;
using nexus_access::Role;
using nexus_access::RouteFamily;
using nexus_access::SessionStatus;
using nexus_access::TreeKind;
using nexus_access::UnlockMethod;
bool local_address(const std::string& address) {
    return address == "127.0.0.1" || address == "::1" || address == "::ffff:127.0.0.1";
}
// A client on the local network (RFC 1918) is a local operator for Nexus. The IPv6
// front reports an IPv4 client as an IPv4-mapped address, so that form is unwrapped first.
bool private_network_address(const std::string& address) {
    if (local_address(address)) return true;
    std::string ipv4 = address;
    if (address.find(':') != std::string::npos) {
        in6_addr mapped{};
        if (inet_pton(AF_INET6, address.c_str(), &mapped) != 1 || !IN6_IS_ADDR_V4MAPPED(&mapped)) return false;
        in_addr unwrapped{};
        std::memcpy(&unwrapped, &mapped.s6_addr[12], sizeof(unwrapped));
        char text[INET_ADDRSTRLEN] = {};
        if (!inet_ntop(AF_INET, &unwrapped, text, sizeof(text))) return false;
        ipv4 = text;
    }
    return NetworkBeacon::isRFC1918(ipv4);
}
class NexusError : public std::runtime_error {
public:
    NexusError(int status, const std::string& message, bool retryable = false)
        : std::runtime_error(message), status(status), retryable(retryable) {}
    int status;
    bool retryable;  // the federation was not reached; the same request may succeed later
    bool identity_revoked{false};  // the network revoked this device's key; no retry succeeds
};
std::string text(const json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : "";
}
bool flag(const json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() && it->get<bool>();
}
json string_list(const json& value) {
    json result = json::array();
    if (!value.is_array()) return result;
    for (const auto& item : value) {
        if (item.is_string() && std::find(result.begin(), result.end(), item) == result.end()) result.push_back(item);
    }
    return result;
}
bool contains(const json& list, const std::string& value) {
    return list.is_array() && std::find(list.begin(), list.end(), value) != list.end();
}
bool contains(const json& list, Permission permission) {
    return contains(list, std::string(nexus_access::name(permission)));
}
json ordered(const json& permissions) {
    json result = json::array();
    for (const auto permission : nexus_access::kPermissions) {
        if (contains(permissions, permission)) result.push_back(nexus_access::name(permission));
    }
    for (const auto& name : permissions) {
        if (!contains(result, name.get<std::string>())) result.push_back(name);
    }
    return result;
}
Role role_from(const std::string& text) {
    const auto role = nexus_access::parse_role(text);
    if (!role) throw NexusError(400, kMsgChooseRole);
    return *role;
}
json permissions_for(const json& assignments, const std::string& key) {
    if (!assignments.is_array()) return json::array();
    for (const auto& entry : assignments) {
        if (entry.is_object() && nexus_access::same_key(text(entry, "management_pubkey"), key)) {
            return string_list(entry.value("permissions", json::array()));
        }
    }
    return json::array();
}
json find_member(const json& members, const std::string& node_id) {
    for (const auto& member : members) {
        if (member.value("node_id", "") == node_id) return member;
    }
    return nullptr;
}
void disable_caching(httplib::Response& res) { res.set_header("Cache-Control", "no-store"); }
void reply_json(httplib::Response& res, int status, const json& body) {
    disable_caching(res);
    res.status = status;
    res.set_content(body.dump(), kJsonContentType);
}
void reply_error(httplib::Response& res, int status, const std::string& message) {
    reply_json(res, status, json{{"error", message}});
}

std::string multipart_body(const httplib::MultipartFormData& form, const std::string& boundary) {
    std::string body;
    for (const auto& entry : form.fields) {
        body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + entry.second.name + "\"\r\n\r\n" +
            entry.second.content + "\r\n";
    }
    for (const auto& entry : form.files) {
        const auto& file = entry.second;
        body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + file.name + "\"; filename=\"" + file.filename +
            "\"\r\nContent-Type: " + (file.content_type.empty() ? "application/octet-stream" : file.content_type) +
            "\r\n\r\n" + file.content + "\r\n";
    }
    return body + "--" + boundary + "--\r\n";
}

// Bridges a streamed peer reply to the client. The peer request runs on its own thread
// so the route handler learns the peer's status before httplib commits the response:
// a refusal before the first body byte keeps its status and JSON body, and a failure
// after that becomes one SSE error event.
class RelayStream {
public:
    ~RelayStream() { abandon(); }
    void start(const std::string& url, const std::string& body, const std::map<std::string, std::string>& headers) {
        worker_ = std::thread([this, url, body, headers] { run(url, body, headers); });
    }
    int wait_for_status(std::string& reply, std::string& content_type) {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [&] { return status_ != 0 || finished_; });
        if (success_locked() && !(finished_ && !forwarded_ && !failure_.empty())) {
            content_type = content_type_;
            return status_;
        }
        ready_.wait(lock, [&] { return finished_; });
        reply = error_body_;
        content_type = content_type_;
        // A 2xx whose transport broke before the first byte delivered nothing either.
        return success_locked() ? 0 : status_;
    }
    bool pull(httplib::DataSink& sink) {
        std::string next;
        std::string failure;
        bool finished = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [&] { return !chunk_.empty() || finished_; });
            next.swap(chunk_);
            finished = finished_;
            failure = failure_;
        }
        ready_.notify_all();
        if (!next.empty() && !sink.write(next.data(), next.size())) { disconnect(); return false; }
        if (!finished) return true;
        if (!failure.empty()) {
            auto payload = ErrorResponse::create(failure, ErrorType::BACKEND_ERROR);
            payload["error"]["status_code"] = 502;
            const auto event = "event: error\ndata: " + payload.dump() + "\n\n";
            sink.write(event.data(), event.size());
        }
        sink.done();
        return true;
    }
    void abandon() {
        disconnect();
        if (worker_.joinable()) worker_.join();
    }
    std::string failure() {
        std::lock_guard<std::mutex> lock(mutex_);
        return failure_;
    }

private:
    static constexpr size_t kMaxErrorBody = 64 * 1024;
    bool success_locked() const { return status_ >= 200 && status_ < 300; }
    void disconnect() {
        { std::lock_guard<std::mutex> lock(mutex_); client_gone_ = true; }
        ready_.notify_all();
    }
    bool client_gone() {
        std::lock_guard<std::mutex> lock(mutex_);
        return client_gone_;
    }
    void announce(int status, const std::map<std::string, std::string>& headers) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = status;
        const auto type = headers.find("content-type");
        if (type != headers.end()) content_type_ = type->second;
    }
    bool push(const char* data, size_t length) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!success_locked()) {
            if (error_body_.size() < kMaxErrorBody) error_body_.append(data, std::min(length, kMaxErrorBody - error_body_.size()));
            return true;
        }
        ready_.wait(lock, [&] { return chunk_.empty() || client_gone_; });
        if (client_gone_) return false;
        chunk_.assign(data, length);
        forwarded_ = true;
        ready_.notify_all();
        return true;
    }
    void run(const std::string& url, const std::string& body, const std::map<std::string, std::string>& headers) {
        std::map<std::string, std::string> response_headers;
        std::string failure;
        int status = 0;
        try {
            const auto result = utils::HttpClient::post_stream(url, body,
                [this](const char* data, size_t length) { return push(data, length); }, headers, kRelayStreamTimeoutSeconds,
                [this, &response_headers](int code) { announce(code, response_headers); },
                utils::HttpSecurityPolicy::TrustedLoopback, [this] { return client_gone(); }, &response_headers);
            status = result.status_code;
            if (result.curl_code != 0) failure = "The device's server closed the stream: " + result.curl_error;
        } catch (const std::exception& e) {
            failure = e.what();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // post_stream reports the status with the first body byte; a reply without a
            // body announces it only here.
            if (status_ == 0 && status != 0) {
                status_ = status;
                const auto type = response_headers.find("content-type");
                if (type != response_headers.end()) content_type_ = type->second;
            }
            if (!client_gone_) failure_ = failure;
            finished_ = true;
        }
        ready_.notify_all();
    }

    std::mutex mutex_;
    std::condition_variable ready_;
    std::thread worker_;
    int status_{0};
    std::string content_type_;
    std::string chunk_;
    std::string error_body_;
    std::string failure_;
    bool forwarded_{false};
    bool finished_{false};
    bool client_gone_{false};
};
}

namespace nexus_access {
std::string_view name(Permission permission) {
    switch (permission) {
    case Permission::Read: return "read";
    case Permission::Write: return "write";
    case Permission::AddChild: return "add_child";
    case Permission::EditNode: return "edit_node";
    case Permission::DeleteNode: return "delete_node";
    case Permission::ExpandSubnet: return "expand_subnet";
    case Permission::ConnectPrivate: return "connect_private";
    case Permission::ConnectShared: return "connect_shared";
    case Permission::RelayForward: return "relay_forward";
    case Permission::RoutingCoordinate: return "routing_coordinate";
    case Permission::StunRespond: return "stun_respond";
    case Permission::RelayRegister: return "relay_register";
    case Permission::AllocateIp: return "allocate_ip";
    case Permission::Admin: return "admin";
    }
    return {};
}

std::optional<Permission> parse_permission(std::string_view text) {
    for (const auto permission : kPermissions) {
        if (name(permission) == text) return permission;
    }
    return std::nullopt;
}

std::string_view name(RouteFamily family) {
    switch (family) {
    case RouteFamily::Inference: return "inference";
    case RouteFamily::Mcp: return "mcp";
    case RouteFamily::ModelsManage: return "models_manage";
    case RouteFamily::Backends: return "backends";
    case RouteFamily::Jobs: return "jobs";
    case RouteFamily::Internal: return "internal";
    case RouteFamily::System: return "system";
    case RouteFamily::Management: return "management";
    }
    return {};
}

std::optional<RouteFamily> parse_family(std::string_view text) {
    for (const auto family : kFamilies) {
        if (name(family) == text) return family;
    }
    return std::nullopt;
}

std::string service_permission(RouteFamily family) {
    return std::string(kServicePermissionPrefix) + std::string(name(family));
}

bool service_permission(const std::string& name) {
    static const std::string prefix = kServicePermissionPrefix;
    return name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0 &&
        valid_route_family(name.substr(prefix.size()));
}

std::string_view name(Role role) {
    switch (role) {
    case Role::Administrator: return "admin";
    case Role::Moderator: return "moderator";
    case Role::User: return "user";
    }
    return {};
}

std::optional<Role> parse_role(std::string_view text) {
    if (text == "administrator" || text == "admin") return Role::Administrator;
    if (text == "moderator") return Role::Moderator;
    if (text == "user" || text == "member") return Role::User;
    return std::nullopt;
}

json role_permissions(Role role) {
    const auto preset = [](std::initializer_list<Permission> permissions, std::initializer_list<RouteFamily> families) {
        json names = json::array();
        for (const auto permission : permissions) names.push_back(name(permission));
        for (const auto family : families) names.push_back(service_permission(family));
        return ordered(names);
    };
    switch (role) {
    case Role::Administrator: {
        json names = json::array({name(Permission::Read), name(Permission::ConnectPrivate), name(Permission::Admin)});
        for (const auto family : kFamilies) names.push_back(service_permission(family));
        return ordered(names);
    }
    case Role::Moderator:
        return preset({Permission::Read, Permission::Write, Permission::AddChild, Permission::ConnectPrivate},
            {RouteFamily::Inference, RouteFamily::Mcp, RouteFamily::ModelsManage, RouteFamily::Jobs});
    case Role::User: break;
    }
    return preset({Permission::Read, Permission::ConnectPrivate}, {RouteFamily::Inference});
}

std::string_view name(NodeType type) {
    switch (type) {
    case NodeType::Root: return "root";
    case NodeType::Customer: return "customer";
    case NodeType::Endpoint: return "endpoint";
    }
    return {};
}

std::optional<NodeType> parse_node_type(std::string_view text) {
    for (const auto type : {NodeType::Root, NodeType::Customer, NodeType::Endpoint}) {
        if (name(type) == text) return type;
    }
    return std::nullopt;
}

std::string_view name(TreeKind kind) {
    switch (kind) {
    case TreeKind::Group: return "group";
    case TreeKind::Node: return "node";
    }
    return {};
}

std::optional<TreeKind> parse_tree_kind(std::string_view text) {
    for (const auto kind : {TreeKind::Group, TreeKind::Node}) {
        if (name(kind) == text) return kind;
    }
    return std::nullopt;
}

std::string_view name(UnlockMethod method) {
    switch (method) {
    case UnlockMethod::Passkey: return "passkey";
    case UnlockMethod::DeviceKey: return "device_key";
    }
    return {};
}

std::optional<UnlockMethod> parse_unlock_method(std::string_view text) {
    for (const auto method : {UnlockMethod::Passkey, UnlockMethod::DeviceKey}) {
        if (name(method) == text) return method;
    }
    return std::nullopt;
}

std::string_view name(ConnectionType type) {
    switch (type) {
    case ConnectionType::Local: return "local";
    case ConnectionType::P2p: return "p2p";
    case ConnectionType::Relay: return "relay";
    case ConnectionType::Unknown: return "unknown";
    }
    return {};
}

std::string_view name(AvatarState state) {
    switch (state) {
    case AvatarState::Ready: return "ready";
    case AvatarState::Unsupported: return "unsupported";
    case AvatarState::Unavailable: return "unavailable";
    }
    return {};
}

std::optional<AvatarState> parse_avatar_state(std::string_view text) {
    for (const auto state : {AvatarState::Ready, AvatarState::Unsupported, AvatarState::Unavailable}) {
        if (name(state) == text) return state;
    }
    return std::nullopt;
}

std::string_view name(SessionStatus status) {
    switch (status) {
    case SessionStatus::Disabled: return "disabled";
    case SessionStatus::Locked: return "locked";
    case SessionStatus::Ready: return "ready";
    case SessionStatus::Joining: return "joining";
    case SessionStatus::Running: return "running";
    case SessionStatus::Degraded: return "degraded";
    }
    return {};
}

std::string_view name(EventType type) {
    switch (type) {
    case EventType::LockChanged: return "nexus.lock_changed";
    case EventType::MeshChanged: return "nexus.mesh_changed";
    case EventType::MemberJoined: return "nexus.member_joined";
    case EventType::MemberLeft: return "nexus.member_left";
    }
    return {};
}

std::string_view name(Operation operation) {
    switch (operation) {
    case Operation::Status: return "status";
    case Operation::PasskeyChallenge: return "passkey/challenge";
    case Operation::KeyHandoff: return "key/handoff";
    case Operation::Devices: return "devices";
    case Operation::Members: return "members";
    case Operation::JoinStatus: return "join/status";
    case Operation::Events: return "events";
    case Operation::Tree: return "tree";
    case Operation::Account: return "account";
    case Operation::Selection: return "selection";
    case Operation::Memberships: return "memberships";
    case Operation::RoutePermissions: return "route-permissions";
    case Operation::Enable: return "enable";
    case Operation::Disable: return "disable";
    case Operation::PasskeyRegister: return "passkey/register";
    case Operation::PasskeyUnlock: return "passkey/unlock";
    case Operation::KeyUnlock: return "key/unlock";
    case Operation::Invites: return "invites";
    case Operation::InvitesCancel: return "invites/cancel";
    case Operation::Join: return "join";
    case Operation::Egress: return "egress";
    case Operation::Lock: return "lock";
    case Operation::Device: return "device";
    case Operation::DeviceCheck: return "device/check";
    case Operation::AccountAvatar: return "account/avatar";
    case Operation::InvitesAccept: return "invites/accept";
    case Operation::AccountDelete: return "account/delete";
    case Operation::Forget: return "forget";
    case Operation::DevicesRemove: return "devices/remove";
    case Operation::DevicesAccess: return "devices/access";
    case Operation::MembersRole: return "members/role";
    case Operation::MembersPermissions: return "members/permissions";
    case Operation::MembershipsLeave: return "memberships/leave";
    case Operation::TreeChildren: return "tree/children";
    case Operation::TreeGroups: return "tree/groups";
    case Operation::TreeRename: return "tree/rename";
    case Operation::TreePermissions: return "tree/permissions";
    case Operation::TreeInvites: return "tree/invites";
    case Operation::TreeMove: return "tree/move";
    case Operation::Relay: return "relay";
    }
    return {};
}

const std::vector<NexusRoute>& nexus_routes() {
    static const std::vector<NexusRoute> routes = {
        {"GET", "status", Operation::Status}, {"GET", "passkey/challenge", Operation::PasskeyChallenge},
        {"GET", "key/handoff", Operation::KeyHandoff}, {"GET", "devices", Operation::Devices},
        {"GET", "members", Operation::Members}, {"GET", "join/status", Operation::JoinStatus},
        {"GET", "events", Operation::Events}, {"GET", "tree", Operation::Tree}, {"GET", "account", Operation::Account},
        {"GET", "selection", Operation::Selection}, {"GET", "memberships", Operation::Memberships},
        {"GET", "route-permissions", Operation::RoutePermissions},
        {"POST", "enable", Operation::Enable}, {"POST", "disable", Operation::Disable},
        {"POST", "passkey/register", Operation::PasskeyRegister}, {"POST", "passkey/unlock", Operation::PasskeyUnlock},
        {"POST", "key/unlock", Operation::KeyUnlock}, {"POST", "invites", Operation::Invites},
        {"POST", "invites/cancel", Operation::InvitesCancel}, {"POST", "join", Operation::Join},
        {"POST", "egress", Operation::Egress}, {"POST", "lock", Operation::Lock}, {"POST", "device", Operation::Device},
        {"POST", "device/check", Operation::DeviceCheck}, {"POST", "account/avatar", Operation::AccountAvatar},
        {"POST", "selection", Operation::Selection}, {"POST", "invites/accept", Operation::InvitesAccept},
        {"POST", "account/delete", Operation::AccountDelete}, {"POST", "forget", Operation::Forget},
        {"POST", "devices/{node_id}/remove", Operation::DevicesRemove},
        {"GET", "devices/{node_id}/access", Operation::DevicesAccess},
        {"POST", "devices/{node_id}/access", Operation::DevicesAccess},
        {"POST", "members/{node_id}/role", Operation::MembersRole},
        {"POST", "members/{node_id}/permissions", Operation::MembersPermissions},
        {"POST", "memberships/{node_id}/leave", Operation::MembershipsLeave},
        {"GET", "tree/{node_id}/children", Operation::TreeChildren},
        {"POST", "tree/{node_id}/groups", Operation::TreeGroups}, {"POST", "tree/{node_id}/rename", Operation::TreeRename},
        {"POST", "tree/{node_id}/permissions", Operation::TreePermissions},
        {"POST", "tree/{node_id}/invites", Operation::TreeInvites}, {"POST", "tree/{node_id}/move", Operation::TreeMove}};
    return routes;
}

bool same_key(const std::string& a, const std::string& b) {
    // The federation accepts either base64 spelling of a key, with or without the ed25519: prefix.
    const auto canonical = [](std::string key) {
        if (key.rfind(kEd25519Prefix, 0) == 0) key.erase(0, kEd25519Prefix.size());
        std::replace(key.begin(), key.end(), '-', '+');
        std::replace(key.begin(), key.end(), '_', '/');
        while (!key.empty() && key.back() == '=') key.pop_back();
        return key;
    };
    const auto left = canonical(a);
    return !left.empty() && left == canonical(b);
}

bool grants(const json& permissions, Permission permission) {
    return contains(permissions, permission) || contains(permissions, Permission::Admin);
}

bool grants(const json& permissions, const std::string& permission) {
    return contains(permissions, permission) || contains(permissions, Permission::Admin);
}

json account_summary(const json& members, const std::string& group_id,
                     const std::string& node_id, const std::string& identity_id) {
    const auto self = find_member(members, node_id);
    if (group_id.empty() || identity_id.empty() || self.is_null() || !self.value("permissions_known", false)) {
        throw NexusError(409, "Account membership could not be verified. Refresh the network and try again.");
    }
    const auto permissions = self.value("permissions", json::array());
    const auto moderator = role_permissions(Role::Moderator);
    // Until Nexus stores named roles, only the complete preset identifies a moderator.
    const bool is_moderator = std::all_of(moderator.begin(), moderator.end(),
        [&](const json& permission) { return contains(permissions, permission.get<std::string>()); });
    const auto role = self.value("is_admin", false) ? Role::Administrator : is_moderator ? Role::Moderator : Role::User;
    return {{"id", group_id}, {"member_id", identity_id}, {"device_node_id", node_id}, {"role", name(role)}};
}

json account_avatar(const json& value, const std::string& group_id) {
    if (!value.is_object() || value.value("group_id", json()) != group_id ||
        !value.contains("revision") || !value["revision"].is_number_integer() ||
        (!value["revision"].is_number_unsigned() && value["revision"].get<int64_t>() < 0) ||
        value["revision"].get<uint64_t>() > kMaxSafeInteger ||
        !value.contains("image") || (!value["image"].is_null() && !value["image"].is_string()) ||
        !value.contains("can_write") || !value["can_write"].is_boolean() ||
        (value.contains("pending") && (!value["pending"].is_boolean() ||
            (value["pending"].get<bool>() && !value["image"].is_null())))) {
        throw NexusError(502, "The network returned invalid account image data");
    }
    if (value["image"].is_string()) {
        const auto& image = value["image"].get_ref<const std::string&>();
        if (image.size() > kMaxAvatarImageBytes || image.rfind(kPngDataUrlPrefix, 0) != 0) {
            throw NexusError(502, "The network returned an invalid profile image");
        }
    }
    return {{"state", name(AvatarState::Ready)}, {"revision", value["revision"]},
        {"image", value["image"]}, {"can_write", value["can_write"]}, {"pending", value.value("pending", false)}};
}

json tree_path(const std::string& id, const std::string& root_id,
               const std::function<json(const std::string&)>& fetch) {
    return tree_path(id, std::vector<std::string>{root_id}, fetch);
}

json tree_path(const std::string& id, const std::vector<std::string>& root_ids,
               const std::function<json(const std::string&)>& fetch) {
    json path = json::array();
    std::set<std::string> visited;
    auto current = id;
    while (!current.empty() && visited.size() < kMaxHierarchyDepth) {
        if (current.size() > kMaxNodeIdLength || current.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos || !visited.insert(current).second) {
            throw NexusError(400, "Invalid group hierarchy");
        }
        auto node = fetch(current);
        if (!node.is_object() || text(node, "id") != current) throw NexusError(502, "The network returned an invalid node");
        const auto type = parse_node_type(text(node, "type"));
        if (!path.empty() && type != NodeType::Customer && type != NodeType::Root) {
            throw NexusError(400, "Only groups can contain other nodes");
        }
        path.push_back(node);
        if (std::find(root_ids.begin(), root_ids.end(), current) != root_ids.end()) return path;
        current = text(node, "parent_id");
    }
    throw NexusError(403, "That node is outside your group or its hierarchy is too deep");
}

json invite_permissions(const json& requested) {
    if (requested.is_null()) return nullptr;
    // The federation refuses any other tree name in an invite; svc: names pass through
    // and the federation reduces them to what the inviter holds.
    static constexpr std::array<Permission, 5> grantable = {Permission::Read, Permission::Write, Permission::AddChild,
        Permission::ConnectPrivate, Permission::ConnectShared};
    if (!requested.is_array() || requested.empty()) throw NexusError(400, kMsgPermissionList);
    json result = json::array();
    for (const auto& name : requested) {
        if (!name.is_string()) throw NexusError(400, kMsgPermissionList);
        const auto value = name.get<std::string>();
        const auto permission = parse_permission(value);
        if (permission == Permission::Admin) throw NexusError(400, "An invite cannot grant admin. Change the member's role after it joins.");
        if ((!permission || std::find(grantable.begin(), grantable.end(), *permission) == grantable.end()) && !service_permission(value)) {
            throw NexusError(400, "An invite cannot grant this permission: " + value);
        }
        if (!contains(result, value)) result.push_back(value);
    }
    return ordered(result);
}

bool selection_key_allowed(const std::string& target, const std::string& peer_key) {
    return peer_key.empty() || target != "auto";
}

std::optional<bool> load_pin_request(bool mesh_peer, const json& pinned, RecipeOptions& options,
                                     std::set<std::string>& masked_saved_keys) {
    if (mesh_peer) {
        options.remove_option("pinned");
        masked_saved_keys.erase("pinned");
        return std::nullopt;
    }
    if (pinned.is_boolean()) return pinned.get<bool>();
    return std::nullopt;
}

const unsigned kCatalogRevision = 2;
namespace {
constexpr size_t kVersionPartDigits = 6;
constexpr uint64_t kVersionPartMax = 999;
constexpr uint64_t kVersionPartSpace = 1000;
constexpr uint64_t kCatalogRevisionSpace = 100;
}

uint64_t catalog_version(const std::string& lemond_version) {
    uint64_t parts[3] = {0, 0, 0};
    size_t pos = 0;
    for (auto& part : parts) {
        const auto start = pos;
        while (pos < lemond_version.size() && lemond_version[pos] >= '0' && lemond_version[pos] <= '9' && pos - start < kVersionPartDigits) {
            part = part * 10 + static_cast<uint64_t>(lemond_version[pos] - '0');
            ++pos;
        }
        if (pos == start) return 0;
        if (&part != &parts[2]) {
            if (pos >= lemond_version.size() || lemond_version[pos] != '.') return 0;
            ++pos;
        }
    }
    if (parts[1] > kVersionPartMax || parts[2] > kVersionPartMax) return 0;
    return ((parts[0] * kVersionPartSpace + parts[1]) * kVersionPartSpace + parts[2]) * kCatalogRevisionSpace + kCatalogRevision;
}

namespace {
struct CatalogClaim {
    bool exact;
    std::string url;
    std::string raw;
};
void for_each_catalog_claim(const nlohmann::ordered_json& routes,
                            const std::function<void(const std::string&, const std::string&, const std::string&, const CatalogClaim&)>& visit) {
    static const char* const quad_prefixes[] = {"/api/v0/", "/api/v1/", "/v0/", "/v1/"};
    for (const auto& route : routes) {
        const auto id = route.value("id", std::string());
        const auto family = route.value("family", std::string());
        if (!valid_route_family(family)) throw std::logic_error("Route '" + id + "' has no valid route policy family");
        const auto prefixes = route.value("prefixes", std::string());
        for (const auto& path_item : route.at("paths")) {
            const auto path = path_item.get<std::string>();
            std::vector<std::string> urls;
            if (prefixes == "Quad") {
                for (const char* prefix : quad_prefixes) urls.push_back(prefix + path);
            } else if (prefixes == "Internal") {
                urls.push_back("/internal/" + path);
            } else {
                urls.push_back(path);
            }
            for (const auto& url : urls) {
                CatalogClaim claim;
                claim.raw = url;
                const auto parameter = url.find('{');
                claim.exact = parameter == std::string::npos;
                claim.url = claim.exact ? url : url.substr(0, parameter);
                while (claim.exact && claim.url.size() > 1 && claim.url.back() == '/') claim.url.pop_back();
                for (const auto& method : route.at("methods")) visit(family, id, method.get<std::string>(), claim);
            }
        }
    }
}
}

nlohmann::ordered_json service_catalog(const nlohmann::ordered_json& routes, const std::string& lemond_version) {
    // The SDK refuses a catalog that lists one method and URL twice, even within a
    // family, so every claim appears once.
    std::set<std::string> prefixes;  // "family METHOD prefix"
    for_each_catalog_claim(routes, [&](const std::string& family, const std::string&, const std::string& method, const CatalogClaim& claim) {
        if (!claim.exact) prefixes.insert(family + " " + method + " " + claim.url);
    });
    nlohmann::ordered_json families = nlohmann::ordered_json::array();
    std::map<std::string, std::string> claims;  // "METHOD path|prefix url", as the SDK keys them -> family
    std::string current_family;
    std::string current_key;
    nlohmann::ordered_json current_methods = nlohmann::ordered_json::array();
    const auto family_routes = [&](const std::string& family) -> nlohmann::ordered_json& {
        for (auto& item : families) {
            if (item.at("name") == family) return item["routes"];
        }
        families.push_back({{"name", family}, {"routes", nlohmann::ordered_json::array()}});
        return families.back()["routes"];
    };
    const auto flush = [&] {
        if (!current_methods.empty()) {
            family_routes(current_family).push_back({{"methods", current_methods},
                {current_key.rfind("path ", 0) == 0 ? "path" : "prefix", current_key.substr(current_key.find(' ') + 1)}});
        }
        current_methods = nlohmann::ordered_json::array();
    };
    for_each_catalog_claim(routes, [&](const std::string& family, const std::string& id, const std::string& method, const CatalogClaim& claim) {
        family_routes(family);
        const std::string kind_and_url = (claim.exact ? "path " : "prefix ") + claim.url;
        if (current_family != family || current_key != kind_and_url) {
            flush();
            current_family = family;
            current_key = kind_and_url;
        }
        const auto key = method + " " + kind_and_url;
        const auto owner = claims.emplace(key, family);
        if (!owner.second && owner.first->second != family) {
            throw std::logic_error("Route '" + id + "' claims " + key + " for family '" + family +
                                   "', which family '" + owner.first->second + "' already serves");
        }
        if (!owner.second) return;
        if (claim.exact && prefixes.count(family + " " + method + " " + claim.raw)) return;
        current_methods.push_back(method);
    });
    flush();
    family_routes("management").push_back({{"methods", {"GET", "POST"}}, {"prefix", nexus_prefix}});
    return {{"version", catalog_version(lemond_version)},
        {"app", {{"name", "lemonade"}, {"version", lemond_version}}},
        {"families", families}};
}

std::string ingress_denied_log_line(const json& event) {
    const auto field = [&](const char* key, size_t limit = 256) {
        std::string value;
        const auto it = event.is_object() ? event.find(key) : event.end();
        if (it == event.end()) return std::string("-");
        if (it->is_string()) value = it->get<std::string>();
        else if (it->is_number_unsigned()) value = std::to_string(it->get<uint64_t>());
        else if (it->is_number_integer()) value = std::to_string(it->get<int64_t>());
        else if (it->is_array()) {
            for (const auto& item : *it) {
                if (!item.is_string()) continue;
                if (!value.empty()) value += ",";
                value += item.get<std::string>();
            }
        }
        if (value.size() > limit) value = value.substr(0, limit) + "...";
        for (auto& c : value) {
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = '?';
        }
        return value.empty() ? std::string("-") : value;
    };
    return "Nexus route policy: " + field("kind", 32) + " " + field("method", 16) + " " + field("path") + " from " +
        field("peer_node_id", 128) + " (family " + field("family", 64) + ", needs " + field("required") + ", catalog " +
        field("catalog_version", 32) + ")";
}

const std::vector<std::string> kPeerHeaders = {kPeerNodeHeader, kPeerKeyHeader, kPeerPermissionsHeader, kFamilyHeader,
    kCatalogVersionHeader};

namespace {
thread_local PeerContext g_request_peer;
}

void accept_peer_headers(httplib::Request& req, bool mesh_ingress) {
    g_request_peer = PeerContext{};
    if (mesh_ingress) {
        g_request_peer.node_id = req.get_header_value(kPeerNodeHeader);
        g_request_peer.family = parse_family(req.get_header_value(kFamilyHeader));
        return;
    }
    for (const auto& name : kPeerHeaders) req.headers.erase(name);
}

const PeerContext& request_peer() {
    return g_request_peer;
}
bool request_from_mesh_peer() { return !request_peer().node_id.empty(); }

bool invite_code(const std::string& code) {
    const std::string_view prefix = kLinkTokenPrefix;
    const bool full_token = code.size() == kLinkTokenLength && code.rfind(prefix, 0) == 0 &&
        code.substr(prefix.size()).find_first_not_of("0123456789abcdef") == std::string::npos;
    return full_token || (code.size() == kInviteCodeDigits && code.find_first_not_of("0123456789") == std::string::npos);
}

json tree_entry(const json& node, const std::string& self_key, bool ancestor_admin) {
    const auto type = parse_node_type(text(node, "type"));
    const bool group = type == NodeType::Customer || type == NodeType::Root;
    const auto assignments = node.value("assignments", json::array());
    const auto permissions = permissions_for(assignments, self_key);
    const bool admin = ancestor_admin || contains(permissions, Permission::Admin);
    const bool access_known = grants(permissions, Permission::EditNode);
    return {{"id", text(node, "id")}, {"parent_id", text(node, "parent_id")},
        {"name", text(node, "hostname")}, {"kind", name(group ? TreeKind::Group : TreeKind::Node)},
        {"node_type", text(node, "type")}, {"tunnel_ip", text(node, "tunnel_ip")},
        {"is_inference", flag(node, "is_inference")}, {"management_pubkey", text(node, "mgmt_pubkey")},
        {"permissions", permissions}, {"can_add_group", group && (admin || grants(permissions, Permission::AddChild))},
        {"can_rename", group && (admin || grants(permissions, Permission::EditNode))},
        {"can_manage", group && admin && access_known}, {"access_known", access_known}, {"is_admin", admin},
        {"assignments", access_known ? assignments : json::array()}};
}

json merge_members(const json& group, const json& children, const json& peers,
                   const std::string& self_key, const std::string& self_node_id, const json& mesh_peers,
                   const json& network) {
    const auto group_acl = group.value("assignments", json::array());
    // The federation returns a node's full assignment list only to callers with edit_node on it.
    const bool group_acl_known = grants(permissions_for(group_acl, self_key), Permission::EditNode);
    const auto creator = text(group, "mgmt_pubkey");
    const auto network_entry = [&](const std::string& id) {
        if (network.is_array()) {
            for (const auto& device : network) {
                if (device.is_object() && text(device, "node_id") == id) return device;
            }
        }
        return json::object();
    };
    const auto peer_entry = [&](const std::string& id) {
        if (peers.is_array()) {
            for (const auto& candidate : peers) {
                if (candidate.is_object() && text(candidate, "node_id") == id) return candidate;
            }
        }
        return json::object();
    };
    const auto connection_type = [&](const std::string& id, bool self, const json& peer) {
        auto type = self ? ConnectionType::Local : ConnectionType::Unknown;
        // Discovery endpoints are candidates; only the dataplane knows the active path.
        if (!self && flag(peer, "is_online") && mesh_peers.is_array()) {
            for (const auto& connection : mesh_peers) {
                if (!connection.is_object() || text(connection, "node_id") != id || !flag(connection, "is_online")) continue;
                if (connection.value("last_rx", int64_t{0}) > 0 || connection.value("last_handshake", int64_t{0}) > 0) {
                    if (connection.contains("path")) {
                        const auto path = text(connection, "path");
                        if (path == "relay") type = ConnectionType::Relay;
                        else if (path == "lan" || path == "direct") type = ConnectionType::P2p;
                    } else if (!text(connection, "relay_endpoint").empty()) type = ConnectionType::Relay;
                    else if (!text(connection, "endpoint").empty()) type = ConnectionType::P2p;
                }
                break;
            }
        }
        return name(type);
    };
    const auto latency = [](const json& peer) {
        return peer.contains("latency_ms") && peer["latency_ms"].is_number() ? peer["latency_ms"].get<double>() : 0.0;
    };
    json members = json::array();
    if (!children.is_array()) return members;
    std::set<std::string> listed;
    for (const auto& child : children) {
        if (!child.is_object() || parse_node_type(text(child, "type")) != NodeType::Endpoint) continue;
        const auto id = text(child, "id");
        if (id.empty()) continue;
        listed.insert(id);
        const auto key = text(child, "mgmt_pubkey");
        const bool self = id == self_node_id;
        const bool permissions_known = group_acl_known || self;
        const auto permissions = permissions_known ? permissions_for(group_acl, key) : json::array();
        const bool owner = same_key(key, creator);
        const auto device_acl = child.value("assignments", json::array());
        const auto own_device = permissions_for(device_acl, self_key);
        const bool access_known = grants(own_device, Permission::EditNode);
        json access = json::array();
        if (access_known && device_acl.is_array()) {
            for (const auto& entry : device_acl) {
                if (!entry.is_object()) continue;
                access.push_back({{"management_pubkey", text(entry, "management_pubkey")},
                    {"permissions", string_list(entry.value("permissions", json::array()))}});
            }
        }
        const auto peer = peer_entry(id);
        const auto listed_device = network_entry(id);
        members.push_back({{"node_id", id}, {"device_name", text(child, "hostname")},
            {"tunnel_ip", text(child, "tunnel_ip")}, {"management_pubkey", key},
            {"permissions", ordered(permissions)}, {"permissions_known", permissions_known},
            {"is_admin", permissions_known ? contains(permissions, Permission::Admin) : owner}, {"is_owner", owner},
            {"is_inference", flag(child, "is_inference")},
            {"access", access}, {"access_known", access_known},
            {"can_remove", !self && grants(own_device, Permission::DeleteNode)},
            {"is_online", self || flag(peer, "is_online")},
            {"connection_type", connection_type(id, self, peer)},
            {"latency_ms", latency(peer)},
            {"member", false}, {"group_role", "home"},
            {"can_connect", self || flag(listed_device, "can_connect")}, {"can_connect_known", self || network.is_array()}});
    }
    // Devices outside this group's children: members that joined it from their own
    // networks, and the devices of networks this identity joined. Their permissions and
    // device access belong to other groups, so they are not reported here.
    if (network.is_array()) {
        for (const auto& device : network) {
            if (!device.is_object()) continue;
            const auto id = text(device, "node_id");
            if (id.empty() || id == self_node_id || !listed.insert(id).second) continue;
            const auto peer = peer_entry(id);
            const auto role = text(device, "group_role");
            members.push_back({{"node_id", id}, {"device_name", text(device, "device_name")},
                {"tunnel_ip", text(device, "tunnel_ip")}, {"management_pubkey", text(device, "mgmt_pubkey")},
                {"permissions", json::array()}, {"permissions_known", false},
                {"is_admin", flag(device, "admin")}, {"is_owner", false},
                {"is_inference", flag(device, "is_inference")},
                {"access", json::array()}, {"access_known", false},
                {"can_remove", false},
                {"is_online", flag(peer, "is_online")},
                {"connection_type", connection_type(id, false, peer)},
                {"latency_ms", latency(peer)},
                {"member", flag(device, "member")}, {"group_role", role.empty() ? "home" : role},
                {"group_id", text(device, "group_id")},
                {"can_connect", flag(device, "can_connect")}, {"can_connect_known", true}});
        }
    }
    return members;
}

json device_access(const json& members, const std::string& device_id, const std::string& self_node_id) {
    const auto target = find_member(members, device_id);
    if (target.is_null()) return nullptr;
    const auto self = find_member(members, self_node_id);
    const bool can_manage = self.is_object() && self.value("is_admin", false);
    const bool access_known = target.value("access_known", false);
    json rows = json::array();
    for (const auto& member : members) {
        const auto id = member.value("node_id", "");
        if (id == device_id || id == self_node_id) continue;
        const auto permissions = permissions_for(target.value("access", json::array()), member.value("management_pubkey", ""));
        rows.push_back({{"node_id", id}, {"device_name", member.value("device_name", "")},
            {"is_admin", member.value("is_admin", false)},
            {"see", grants(permissions, Permission::Read)}, {"connect", grants(permissions, Permission::ConnectPrivate)},
            {"editable", can_manage && access_known && !contains(permissions, Permission::Admin)}});
    }
    return {{"node_id", device_id}, {"device_name", target.value("device_name", "")},
        {"access_known", access_known}, {"can_manage", can_manage}, {"members", rows}};
}

json group_permissions(const json& requested, bool admin) {
    if (!requested.is_array()) throw NexusError(400, kMsgPermissionList);
    json result = json::array({name(Permission::Read)});
    for (const auto& item : requested) {
        if (!item.is_string()) throw NexusError(400, kMsgPermissionList);
        const auto value = item.get<std::string>();
        const auto permission = parse_permission(value);
        if (permission == Permission::Admin) throw NexusError(400, "Change the member's role to grant or remove admin");
        if (!permission && !service_permission(value)) throw NexusError(400, "Unknown permission: " + value);
        if (!contains(result, value)) result.push_back(value);
    }
    if (admin) result.push_back(name(Permission::Admin));
    return ordered(result);
}

json device_permissions(const json& current, bool see, bool connect) {
    json result = json::array();
    for (const auto& item : string_list(current)) {
        const auto permission = parse_permission(item.get<std::string>());
        if (permission != Permission::Read && permission != Permission::ConnectPrivate) result.push_back(item);
    }
    if (see || connect) result.push_back(name(Permission::Read));
    if (connect) result.push_back(name(Permission::ConnectPrivate));
    return ordered(result);
}

std::pair<int, std::string> assignment_error(const std::string& reason) {
    if (reason.find("only an admin") != std::string::npos) return {403, kMsgOnlyGroupAdmin};
    if (reason.find("cannot change its own entry") != std::string::npos) return {403, "You cannot change your own admin access"};
    if (reason.find("has not joined") != std::string::npos) return {403, "That device has not joined your group"};
    if (reason.rfind("unknown permission", 0) == 0) return {400, "The network does not recognize that permission"};
    if (reason == "invalid principal") return {400, "That member has an invalid device key"};
    if (reason == "node not found") return {404, "That group or device no longer exists"};
    if (reason == "failed to persist node") return {502, "The network could not save the change. Try again."};
    if (reason == "server unreachable" || reason.empty()) return {502, "Cannot reach the network. Try again."};
    return {502, "The network refused the change: " + reason};
}

uint64_t newer_tier1_epoch(const json& membership, uint64_t saved) {
    if (!membership.is_object()) return 0;
    const auto epoch = membership.find("epoch");
    if (epoch == membership.end() || !epoch->is_number_integer() ||
        (!epoch->is_number_unsigned() && epoch->get<int64_t>() < 0)) return 0;
    const auto value = epoch->get<uint64_t>();
    return value > saved ? value : 0;
}

bool session_dropped(const json& mesh_status) {
    return mesh_status.is_object() && mesh_status.value("needs_rejoin", false);
}

IdentitySource identity_source(const ProcessRole& role) {
    switch (role.os) {
    case ProcessRole::Os::Windows: return IdentitySource::User;
    case ProcessRole::Os::MacOS: return role.root ? IdentitySource::Handoff : IdentitySource::User;
    case ProcessRole::Os::Linux: break;
    }
    return role.root || role.state_directory ? IdentitySource::System : IdentitySource::User;
}

ProcessRole current_process_role() {
    ProcessRole role{};
#ifdef _WIN32
    role.os = ProcessRole::Os::Windows;
#else
#ifdef __APPLE__
    role.os = ProcessRole::Os::MacOS;
#else
    role.os = ProcessRole::Os::Linux;
#endif
    role.root = geteuid() == 0;
#endif
    role.state_directory = !utils::get_environment_variable_utf8("STATE_DIRECTORY").empty();
    return role;
}

const char* identity_source_name(IdentitySource source) {
    switch (source) {
    case IdentitySource::System: return "system";
    case IdentitySource::User: return "user";
    case IdentitySource::Handoff: return "handoff";
    }
    return "system";
}

std::string account_file_tag(const std::string& pubkey) {
    auto key = pubkey;
    if (key.rfind(kEd25519Prefix, 0) == 0) key.erase(0, kEd25519Prefix.size());
    std::replace(key.begin(), key.end(), '+', '-');
    std::replace(key.begin(), key.end(), '/', '_');
    while (!key.empty() && key.back() == '=') key.pop_back();
    if (key.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos) return "";
    return key.substr(0, kAccountTagLength);
}

json account_for_key(const json& previous, const json& archived, const std::string& pubkey) {
    json next = archived.is_object() ? archived : json::object();
    for (const char* name : {"server", "genesis_pubkey", "tier1_epoch", "tier1_chain", "device_name", "mesh_udp_port"}) {
        if (!next.contains(name) && previous.is_object() && previous.contains(name)) next[name] = previous.at(name);
    }
    next["public_key"] = pubkey;
    return next;
}

std::string mesh_transport_failure(bool started, int port, const json& mesh_status) {
    if (!started) return "Could not start the mesh transport";
    int bound = 0;
    if (mesh_status.is_object()) {
        const auto it = mesh_status.find("dataplane_port");
        if (it != mesh_status.end() && it->is_number_integer()) bound = it->get<int>();
    }
    if (port != 0 && bound != port) {
        return "Could not bind the mesh dataplane to UDP port " + std::to_string(port) + ". Choose a free port, or 0 for any port.";
    }
    return {};
}

AccountMatch account_match(const json& account, const std::string& pubkey, const std::string& user_id) {
    if (!account.is_object() || !account.contains("user_id") || account.at("user_id") == user_id) return AccountMatch::Own;
    return account.value("public_key", std::string{}) == pubkey ? AccountMatch::Conflict : AccountMatch::Stale;
}

std::string stale_account_file(const json& account) {
    const auto key = account.is_object() ? account.value("public_key", std::string{}) : std::string{};
    if (!key.empty()) return "account-" + account_file_tag(key) + ".json";
    const auto user = account.is_object() ? account.value("user_id", std::string{}) : std::string{};
    const auto tag = account_file_tag(user);
    return "account-user-" + (tag.empty() ? std::string("unknown") : tag) + ".json";
}

std::pair<json, json> default_invite_permissions(const json& held, bool admin) {
    json granted = json::array();
    json dropped = json::array();
    for (const auto& permission : role_permissions(Role::User)) {
        if (admin || grants(held, permission.get<std::string>())) granted.push_back(permission);
        else dropped.push_back(permission);
    }
    if (granted.empty()) granted = nullptr;
    return {granted, dropped};
}

bool constant_time_equals(const std::string& candidate, const std::string& expected) {
    unsigned char difference = candidate.size() == expected.size() ? 0 : 1;
    for (size_t i = 0; i < expected.size(); ++i) {
        const auto left = i < candidate.size() ? static_cast<unsigned char>(candidate[i]) : 0;
        difference |= static_cast<unsigned char>(left ^ static_cast<unsigned char>(expected[i]));
    }
    return difference == 0;
}

bool bearer_matches(const std::string& authorization, const std::string& api_key, const std::string& admin_key) {
    const std::string prefix = "Bearer ";
    if (authorization.size() <= prefix.size() || authorization.compare(0, prefix.size(), prefix) != 0) return false;
    const auto token = authorization.substr(prefix.size());
    const bool regular = !api_key.empty() && constant_time_equals(token, api_key);
    const bool admin = !admin_key.empty() && constant_time_equals(token, admin_key);
    return regular || admin;
}

bool remote_operation(Operation operation) {
    switch (operation) {
    case Operation::Status: case Operation::JoinStatus: case Operation::Devices: case Operation::Members:
    case Operation::MembersRole: case Operation::MembersPermissions: case Operation::DevicesAccess:
    case Operation::DevicesRemove: case Operation::Invites: case Operation::InvitesCancel: case Operation::DeviceCheck:
    case Operation::Tree: case Operation::TreeChildren: case Operation::TreeGroups: case Operation::TreeRename:
    case Operation::TreePermissions: case Operation::TreeInvites: case Operation::TreeMove: case Operation::RoutePermissions:
        return true;
    case Operation::PasskeyChallenge: case Operation::KeyHandoff: case Operation::Events: case Operation::Account:
    case Operation::Selection: case Operation::Memberships: case Operation::Enable: case Operation::Disable:
    case Operation::PasskeyRegister: case Operation::PasskeyUnlock: case Operation::KeyUnlock: case Operation::Join:
    case Operation::Egress: case Operation::Lock: case Operation::Device: case Operation::AccountAvatar:
    case Operation::InvitesAccept: case Operation::AccountDelete: case Operation::Forget: case Operation::MembershipsLeave:
    case Operation::Relay:
        return false;
    }
    return false;
}

bool nexus_request(const httplib::Request& req) {
    return req.path.rfind(nexus_prefix, 0) == 0;
}

bool administers_tree(const json& path, const std::string& key) {
    if (key.empty() || !path.is_array()) return false;
    for (const auto& node : path) {
        for (const auto& entry : node.value("assignments", json::array())) {
            if (same_key(text(entry, "management_pubkey"), key) &&
                contains(entry.value("permissions", json::array()), Permission::Admin)) return true;
        }
    }
    return false;
}

namespace {
std::pair<int, std::string> service_access(const json& permissions, RouteFamily family, bool management) {
    if (!grants(permissions, Permission::ConnectPrivate) || !grants(permissions, service_permission(family))) {
        return {403, "This Nexus caller lacks connection or service permission"};
    }
    if (management && (family != RouteFamily::Management || !contains(permissions, Permission::Admin))) {
        return {403, "Nexus administration requires an administrator"};
    }
    return {0, ""};
}
}

json route_permissions(const nlohmann::ordered_json& routes) {
    json result = {{"roles", json::array()}, {"routes", json::array()}};
    for (const auto role : kRoles) {
        result["roles"].push_back({{"id", name(role)}, {"permissions", role_permissions(role)}});
    }
    const auto append = [&](const std::string& family, const std::string& method, const json& paths, bool management) {
        json allowed = json::array();
        const auto known = parse_family(family);
        for (const auto& role : result["roles"]) {
            if (known && service_access(role.at("permissions"), *known, management).first == 0) allowed.push_back(role.at("id"));
        }
        result["routes"].push_back({{"family", family}, {"method", method}, {"paths", paths}, {"roles", allowed}});
    };
    for (const auto& route : routes) {
        if (route.value("prefixes", "") == "Internal") continue;
        for (const auto& path : route.at("paths")) {
            auto single = route;
            single["paths"] = nlohmann::ordered_json::array({path});
            std::map<std::string, json> paths_by_method;
            for_each_catalog_claim(nlohmann::ordered_json::array({single}),
                [&](const std::string&, const std::string&, const std::string& method, const CatalogClaim& claim) {
                    auto& paths = paths_by_method[method];
                    if (paths.is_null()) paths = json::array();
                    paths.push_back(claim.raw);
                });
            for (const auto& entry : paths_by_method) append(route.at("family"), entry.first, entry.second, false);
        }
    }
    for (const auto& route : nexus_routes()) {
        if (!remote_operation(route.operation)) continue;
        append(std::string(name(RouteFamily::Management)), route.method, json::array({nexus_prefix + std::string(route.path)}), true);
    }
    return result;
}

std::pair<int, std::string> mesh_request_access(const httplib::Request& req, bool management) {
    if (!local_address(req.remote_addr)) return {403, "Nexus identity must come from the mesh ingress"};
    for (const auto& name : kPeerHeaders) {
        if (req.get_header_value_count(name) != 1 || req.get_header_value(name).empty()) {
            return {403, "A verified Nexus caller identity is required"};
        }
    }
    const auto family = parse_family(req.get_header_value(kFamilyHeader));
    if (!family) return {403, "Unknown Nexus service family"};
    json permissions = json::array();
    const auto names = req.get_header_value(kPeerPermissionsHeader);
    size_t start = 0;
    while (start < names.size()) {
        const auto end = names.find(',', start);
        permissions.push_back(names.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return service_access(permissions, *family, management);
}

std::pair<int, std::string> route_access(NexusListener listener, const httplib::Request& req, Operation operation) {
    const bool loopback = local_address(req.remote_addr);
    if (listener == NexusListener::MeshIngress) {
        const auto access = mesh_request_access(req, true);
        if (access.first != 0) return access;
        return remote_operation(operation) ? std::make_pair(0, std::string()) :
            std::make_pair(403, std::string("Use the local controller for identity, lifecycle and relay operations"));
    }
    if (loopback) return {0, ""};
    if (listener == NexusListener::Management) return {403, "Local network management only"};
    if (private_network_address(req.remote_addr)) return {0, ""};
    return {403, "Connect through the authenticated Nexus mesh. Initial setup requires the local controller, SSH forwarding or a client on the local network."};
}

const char* const kPeerAuthorizationHeader = "X-Lemonade-Peer-Authorization";

std::pair<int, std::string> relay_target(const json& members, const std::string& self_node_id, const std::string& node_id) {
    if (node_id == self_node_id) return {400, "Use this device directly instead of relaying to it"};
    const auto target = find_member(members, node_id);
    if (target.is_null()) return {404, kMsgNotInGroup};
    if (!target.value("is_inference", false)) return {409, "That device is not a Lemonade server"};
    // The federation forwards mesh packets only between devices that may connect, so a
    // refused device would also time out; the clear refusal comes first. Without the
    // network list the flag is unknown and the online state decides.
    if (target.value("can_connect_known", false) && !target.value("can_connect", false)) {
        return {409, "This device is not allowed to connect to that device. Ask an admin to grant it Connect access."};
    }
    if (!target.value("is_online", false)) return {409, "That device is offline or not reachable from this device"};
    auto address = target.value("tunnel_ip", "");
    if (const auto slash = address.find('/'); slash != std::string::npos) address.resize(slash);
    if (address.empty()) return {409, "That device has no mesh address"};
    return {0, address};
}

namespace {
// The GUI aborts a host GET at 35 s, so a stalled health or models poll must not hold a
// lemond worker for the client's 300 s default. A large model takes minutes to load, so
// the relay waits an hour, which also releases the worker of a transport that died
// without a close.
constexpr long kRelayGetTimeoutSeconds = 30;
constexpr long kRelayPostTimeoutSeconds = 3600;

// The first request over a fresh mesh path, as right after lemond or the device started,
// often meets a connection that is still forming. The last repeat runs over a new mesh
// bridge: one whose connections failed this often is dead, for example after its mesh
// path changed.
constexpr std::array<int, 3> kRelayRetryDelaysMs{1000, 2000, 3000};

void relay_failed(httplib::Response& res, const std::string& node_id, const std::string& method, const std::string& path,
                  const RelayFailure& failure) {
    LOG(WARNING, kLogTag) << "Relay to " << node_id << " failed: " << method << " " << path.substr(0, path.find('?'))
                          << ": " << failure.detail << std::endl;
    res.status = failure.status;
    res.set_content(json{{"error", failure.error}, {"detail", failure.detail}}.dump(), kJsonContentType);
}
}

bool relay_retryable(const std::string& reason) {
    // libcurl's texts for a connection that was refused, reset or closed before an answer;
    // a timeout or an HTTP answer is not among them.
    for (const char* text : {"Server returned nothing", "Couldn't connect", "Connection reset", "Recv failure", "Failure when receiving",
                             "Empty reply"}) {
        if (reason.find(text) != std::string::npos) return true;
    }
    return false;
}

RelayFailure relay_failure(int curl_code, const std::string& reason, long waited_seconds) {
    const auto detail = reason.empty() ? std::string("no answer from the device") : reason;
    if (curl_code == CURLE_OPERATION_TIMEDOUT) {
        return {504, "That device's server did not answer within " + std::to_string(waited_seconds) + " seconds", detail};
    }
    return {502, "Cannot reach that device's server through the mesh", detail};
}

void relay_http(uint16_t port, const std::string& node_id, const std::string& path, const httplib::Request& req, httplib::Response& res,
                const std::string& peer_authorization, const std::string& request_body, const std::function<uint16_t()>& reopen) {
    std::map<std::string, std::string> headers;
    if (!peer_authorization.empty()) headers["Authorization"] = peer_authorization;
    if (req.has_header(kClientSessionHeader)) headers[kClientSessionHeader] = req.get_header_value(kClientSessionHeader);
    disable_caching(res);
    const bool read = req.method == "GET";
    const long timeout = read ? kRelayGetTimeoutSeconds : kRelayPostTimeoutSeconds;
    auto body = request_body;
    bool streamed = false;
    if (!read) {
        if (req.is_multipart_form_data()) {
            std::random_device random;
            const auto boundary = "lemonade-relay-" + std::to_string(random()) + std::to_string(random()) + std::to_string(random());
            headers["Content-Type"] = "multipart/form-data; boundary=" + boundary;
            body = multipart_body(req.form, boundary);
        } else {
            headers["Content-Type"] = req.has_header("Content-Type") ? req.get_header_value("Content-Type") : kJsonContentType;
        }
        const auto request = json::parse(body, nullptr, false);
        streamed = request.is_object() && request.value("stream", false);
    }
    // A request whose connection was refused, reset or closed before any answer never
    // reached the device's server, or that server closed before it read the request;
    // a read and a POST (a model load, a chat) are both repeated then.
    for (size_t attempt = 0;; ++attempt) {
        const auto url = nexus_user_key::loopback_url(port) + path;
        RelayFailure failure;
        try {
            if (read) {
                const auto reply = utils::HttpClient::get(url, headers, timeout, utils::HttpSecurityPolicy::TrustedLoopback);
                res.status = reply.status_code;
                res.set_content(reply.body, kJsonContentType);
                return;
            }
            if (streamed) {
                auto stream = std::make_shared<RelayStream>();
                stream->start(url, body, headers);
                std::string reply;
                std::string type;
                const auto status = stream->wait_for_status(reply, type);
                if (status != 0) {
                    if (status < 200 || status >= 300) {
                        res.status = status;
                        res.set_content(reply, type.empty() ? kJsonContentType : type);
                        return;
                    }
                    res.set_chunked_content_provider(type.empty() ? kEventStreamContentType : type,
                        [stream](size_t, httplib::DataSink& sink) { return stream->pull(sink); },
                        [stream](bool) { stream->abandon(); });
                    return;
                }
                failure = relay_failure(0, stream->failure(), timeout);
            } else {
                const auto reply = utils::HttpClient::post(url, body, headers, timeout, utils::HttpSecurityPolicy::TrustedLoopback);
                const auto type = reply.headers.find("content-type");
                res.status = reply.status_code;
                res.set_content(reply.body, type != reply.headers.end() ? type->second : kJsonContentType);
                return;
            }
        } catch (const utils::HttpClientException& e) {
            failure = relay_failure(e.curl_code(), e.what(), timeout);
        } catch (const std::exception& e) {
            failure = relay_failure(0, e.what(), timeout);
        }
        if (attempt >= kRelayRetryDelaysMs.size() || !relay_retryable(failure.detail)) {
            relay_failed(res, node_id, req.method, path, failure);
            return;
        }
        LOG(INFO, kLogTag) << "Relay to " << node_id << " found no answer (" << failure.detail << "); trying again in "
                           << kRelayRetryDelaysMs[attempt] << " ms" << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(kRelayRetryDelaysMs[attempt]));
        if (attempt + 1 == kRelayRetryDelaysMs.size() && reopen) {
            if (const auto fresh = reopen(); fresh != 0) {
                LOG(INFO, kLogTag) << "Relay to " << node_id << " continues over a new mesh bridge" << std::endl;
                port = fresh;
            }
        }
    }
}

}

namespace nexus_access {
namespace {
// The C ABI reports every ln_auth_ed25519 failure as LN_ERR_AUTH, so the texts the SDK
// gives an unreachable federation on that path are recognized as well: its Tier 1
// directory found no server to verify, or the challenge never got an answer.
bool federation_unreachable(const std::string& message) {
    for (const char* reason : {"no verified Tier 1 server", "no server returned a verifiable authority chain", "challenge request failed"}) {
        if (message.find(reason) != std::string::npos) return true;
    }
    return false;
}
// Genesis key discovery reached the servers but fewer than the required number agreed
// inside the network domain. The SDK reports it without an HTTP status, as LN_ERR_CONNECT,
// yet a retry changes nothing: the network's DNS or an explicit key does.
bool genesis_discovery_short(const std::string& message) {
    return message.find("Genesis key discovery needs") != std::string::npos && message.find("agreeing servers") != std::string::npos;
}
}
SdkFailure sdk_failure(int code, const json& reply, int rejected_status) {
    const auto fallback = "Network request failed (SDK error " + std::to_string(code) + ")";
    const auto message = reply.is_object() ? reply.value("error", fallback) : fallback;
    if (message == "rate limited" || message == "too many requests") return {429, "The network is busy. Try again shortly.", true};
    if (genesis_discovery_short(message)) {
        return {502, message + ". Check that the network's DNS names its servers and that this device reaches them over HTTPS, "
                              "or set LEMONADE_NEXUS_GENESIS_PUBKEY to the network's Genesis key.", false};
    }
    // The SDK answers LN_ERR_CONNECT for no transport, a timeout and a 5xx. Neither that
    // nor an unreachable federation refuses this device.
    if (code == LN_ERR_CONNECT || federation_unreachable(message)) return {503, message, true};
    const int status = code == LN_ERR_AUTH ? 403 : code == LN_ERR_NOT_FOUND ? 404 : code == LN_ERR_REJECTED ? rejected_status : 502;
    SdkFailure failure{status, message, false};
    failure.identity_revoked = reply.is_object() && reply.value("identity_revoked", false);
    return failure;
}
// The vendored SDK (UPSTREAM.json) carries the route policy interface; the SDK
// enforces the catalog at the mesh ingress once lemond registers it.
bool route_policy_available() { return true; }
std::pair<int, std::string> handoff_failure(int code) {
    if (code == LN_ERR_PARSE || code == LN_ERR_NULL_ARG) return {400, "The identity envelope is malformed"};
    if (code == LN_ERR_AUTH || code == LN_ERR_REJECTED) {
        return {403, "The identity envelope does not open for the outstanding hand-off request, or the request expired"};
    }
    return {500, "The identity envelope could not be opened (SDK error " + std::to_string(code) + ")"};
}
}
namespace {
json sdk_call(const std::function<ln_error_t(char**)>& operation, int rejected_status = 502) {
    char* output = nullptr;
    const auto code = operation(&output);
    std::unique_ptr<char, decltype(&ln_free)> owned(output, ln_free);
    auto value = output ? json::parse(output, nullptr, false) : json::object();
    if (value.is_discarded()) throw NexusError(502, kMsgInvalidData);
    if (code != LN_OK) {
        const auto failure = nexus_access::sdk_failure(code, value, rejected_status);
        NexusError error(failure.status, failure.message, failure.retryable);
        error.identity_revoked = failure.identity_revoked;
        throw error;
    }
    return value;
}
std::string sdk_string(char* value) {
    std::unique_ptr<char, decltype(&ln_free)> owned(value, ln_free);
    return value ? value : "";
}
}
struct NexusManager::Session {
    using IdentityPtr = std::unique_ptr<ln_identity_t, decltype(&ln_identity_destroy)>;
    std::unique_ptr<ln_client_t, decltype(&ln_destroy)> client{nullptr, ln_destroy};
    IdentityPtr identity{nullptr, ln_identity_destroy};
    std::shared_ptr<ClientSlot> client_slot;
    const IdentitySource source;
    std::unique_ptr<ln_handoff_t, decltype(&ln_identity_handoff_destroy)> handoff{nullptr, ln_identity_handoff_destroy};
    json identity_store = nullptr;  // the SDK's user store status at the last load attempt (User only)
    std::chrono::steady_clock::time_point store_retry_at{};
    std::chrono::seconds store_retry_delay{kRetryDelayInitial};
    std::filesystem::path root;
    int ingress_port;
    int local_service_port;
    int mesh_port;
    bool genesis_key_set{false};
    std::string mesh_port_error;
    json account = json::object();
    bool locked{true};
    bool hold_locked{false};  // an explicit lock stops key unlock until the next key unlock or restart
    bool joined{false};
    bool reconnect{false};
    bool avatar_configured{false};
    std::chrono::steady_clock::time_point retry_at{};
    std::chrono::seconds retry_delay{kRetryDelayInitial};
    std::map<std::string, std::string> egress_ips;
    std::string node_id;
    std::string group_id;
    std::string tunnel_ip;
    std::string domain{"lemonade-nexus.io"};
    std::string region{"us-west"};  // SEIP region label
    std::string host;               // bootstrap server: configured, remembered, or discovered
    std::string rp_id;              // configured, else the network's own value from /api/health
    std::string pubkey;
    std::string revoked_pubkey;  // the key the network revoked, until another key is in use
    SessionStatus detail{SessionStatus::Locked};
    std::string error;
    bool error_retryable{false};  // `error` is a failure to reach the federation, not a refusal
    bool refounded{false};        // the servers serve another chain under the pinned key
    json devices = json::array();
    json join_membership = nullptr;
    std::chrono::system_clock::time_point last_poll{std::chrono::system_clock::now()};
    const nlohmann::ordered_json catalog;  // mesh route catalog for vport 13305, or null
    std::string catalog_error;             // the SDK refused the catalog at the last join

    Session(const std::string& data_root, int port, int local_port, int mesh_listener_port, nlohmann::ordered_json service_catalog,
            const ProcessRole& role, std::shared_ptr<ClientSlot> slot)
        : source(nexus_access::identity_source(role)), root(utils::path_from_utf8(data_root)), ingress_port(port),
          local_service_port(local_port), mesh_port(mesh_listener_port), catalog(std::move(service_catalog)),
          client_slot(std::move(slot)) {
        std::filesystem::create_directories(root);
#ifndef _WIN32
        std::filesystem::permissions(root, std::filesystem::perms::owner_all);
#endif
        std::ifstream input(root / kAccountFile);
        if (input) {
            auto saved = json::parse(input, nullptr, false);
            if (!saved.is_object()) throw NexusError(500, "Device account is invalid; restore account.json");
            account = saved;
            note_legacy_join();
        }
        if (const auto value = utils::get_environment_variable_utf8("LEMONADE_NEXUS_DOMAIN"); !value.empty()) domain = value;
        if (const auto value = utils::get_environment_variable_utf8("LEMONADE_NEXUS_REGION"); !value.empty()) region = value;
        if (const auto value = utils::get_environment_variable_utf8("LEMONADE_NEXUS_SERVER"); !value.empty()) host = value;
        if (const auto value = utils::get_environment_variable_utf8("LEMONADE_NEXUS_RP_ID"); !value.empty()) rp_id = value;
        int public_port = kDefaultServerPort;
        const auto configured_port = utils::get_environment_variable_utf8("LEMONADE_NEXUS_PORT");
        if (!configured_port.empty()) public_port = std::stoi(configured_port);
        if (public_port < 1 || public_port > kMaxPort) throw NexusError(400, "Invalid Nexus server port");
        // The servers' certificates name their own FQDNs, not the network apex, so
        // without a configured host the bootstrap server comes from DNS discovery.
        if (host.empty()) host = account.value("server", std::string{});
        if (host.empty()) {
            char* out = nullptr;
            const auto code = ln_discover_servers(domain.c_str(), region.c_str(), static_cast<uint16_t>(public_port), &out);
            std::unique_ptr<char, decltype(&ln_free)> owned(out, ln_free);
            const auto found = out ? json::parse(out, nullptr, false) : json();
            const auto servers = found.is_object() ? found.value("servers", json::array()) : json::array();
            if (code != LN_OK || servers.empty() || !servers.front().is_object()) {
                throw NexusError(503, "No network server found for " + region + ".seip." + domain +
                                          (found.is_object() ? ": " + found.value("error", std::string{}) : std::string{}));
            }
            host = servers.front().value("host", std::string{});
            account["server"] = host;
            persist();
        }
        withdraw_client();
        client.reset(ln_create_tls(host.c_str(), static_cast<uint16_t>(public_port)));
        if (!client) throw NexusError(500, "Cannot create Nexus client");
        publish_client();
        // The region's SEIP records give the SDK more candidate servers for Genesis key
        // discovery and Tier 1 refresh when it knows fewer than three hosts.
        if (const auto code = ln_set_discovery_target(client.get(), domain.c_str(), region.c_str()); code != LN_OK) {
            LOG(WARNING, kLogTag) << "The SDK refused the discovery target " << region << ".seip." << domain
                                  << " (SDK error " << code << "); discovery uses the known servers only" << std::endl;
        }
        ln_set_session_moved_callback(client.get(), [](char* event_json, void*) {
            std::unique_ptr<char, decltype(&ln_free)> owned(event_json, ln_free);
            LOG(INFO, kLogTag) << "Session moved: " << (event_json ? event_json : "") << std::endl;
        }, nullptr);
        // The SDK answers a refused mesh request itself and reports it here, on an SDK
        // thread: one log line per event, no SDK call besides releasing the text.
        ln_set_ingress_denied_callback(client.get(), [](char* event_json, void*) {
            std::unique_ptr<char, decltype(&ln_free)> owned(event_json, ln_free);
            const auto event = event_json ? json::parse(event_json, nullptr, false) : json();
            const auto line = nexus_access::ingress_denied_log_line(event.is_object() ? event : json::object());
            if (event.is_object() && event.value("kind", "") == "denied") LOG(WARNING, kLogTag) << line << std::endl;
            else LOG(INFO, kLogTag) << line << std::endl;
        }, nullptr);
        load_revoked_key();
        if (source == IdentitySource::System) {
            load_system_identity();
        } else if (source == IdentitySource::User) {
            load_user_identity();
        }
        // Tree, session and private-API calls go only to Tier 1 servers verified from this key.
        if (const auto genesis = utils::get_environment_variable_utf8("LEMONADE_NEXUS_GENESIS_PUBKEY"); !genesis.empty()) {
            if (ln_set_genesis_key(client.get(), genesis.c_str(), 0) != LN_OK) {
                throw NexusError(400, "LEMONADE_NEXUS_GENESIS_PUBKEY is not a base64 Ed25519 public key");
            }
            genesis_key_set = true;
        } else if (const auto pinned = account.value("genesis_pubkey", std::string{}); !pinned.empty()) {
            if (ln_set_genesis_key(client.get(), pinned.c_str(), 0) != LN_OK) {
                throw NexusError(500, "Saved Genesis key is invalid; restore account.json");
            }
            genesis_key_set = true;
        }
        if (genesis_key_set) apply_epoch_floor();
    }
    ~Session() {
        withdraw_client();
        if (client) ln_mesh_disable(client.get());
    }
    void publish_client() {
        std::lock_guard<std::mutex> lock(client_slot->mutex);
        client_slot->client = client.get();
    }
    void withdraw_client() {
        std::lock_guard<std::mutex> lock(client_slot->mutex);
        client_slot->client = nullptr;
    }
    void adopt_identity(IdentityPtr next) {
        const auto key = sdk_string(ln_identity_pubkey(next.get()));
        if (key.empty()) throw NexusError(500, "Cannot read the device identity");
        if (key == revoked_pubkey) throw NexusError(409, "The network revoked that device key; create a new key and unlock again");
        if (identity) {
            if (key == pubkey) return;
            lock();
            hold_locked = false;
            node_id.clear();
            group_id.clear();
            tunnel_ip.clear();
            retry_at = {};
        }
        if (ln_set_identity(client.get(), next.get()) != LN_OK) throw NexusError(500, "Cannot load device identity");
        identity = std::move(next);
        pubkey = key;
        forget_revoked_key();
        switch_account(key);
        const auto avatar_cache = utils::path_to_utf8(root / "avatar-cache");
        avatar_configured = ln_account_avatar_configure(client.get(), identity.get(), avatar_cache.c_str()) == LN_OK;
        clear_error();
        LOG(INFO, kLogTag) << "Device identity in use: " << pubkey << " (" << nexus_access::identity_source_name(source) << ")" << std::endl;
    }
    void load_system_identity() {
        const auto path = utils::path_to_utf8(root / kIdentityFile);
        IdentityPtr loaded(ln_identity_load(path.c_str()), ln_identity_destroy);
        if (!loaded) {
            if (std::filesystem::exists(root / kIdentityFile)) throw NexusError(500, "Device identity is invalid; restore identity.json");
            loaded.reset(ln_identity_generate());
            if (!loaded || ln_identity_save(loaded.get(), path.c_str()) != LN_OK) throw NexusError(500, "Cannot save device identity");
        }
        adopt_identity(std::move(loaded));
    }
    // A store that is locked, refused or unsupported leaves the device without an
    // identity; there is no plaintext fallback. The next poll tries again with backoff.
    void load_user_identity() {
        ln_error_t code = LN_OK;
        IdentityPtr loaded(ln_identity_user_load(&code), ln_identity_destroy);
        if (loaded && !revoked_pubkey.empty() && sdk_string(ln_identity_pubkey(loaded.get())) == revoked_pubkey) {
            loaded.reset();
            code = ln_identity_user_remove() == LN_OK ? LN_ERR_NOT_FOUND : LN_ERR_REJECTED;
        }
        if (!loaded && code == LN_ERR_NOT_FOUND) loaded.reset(ln_identity_user_create(&code));
        char* out = nullptr;
        if (ln_identity_user_status(&out) == LN_OK && out) {
            auto status = json::parse(out, nullptr, false);
            if (status.is_object()) identity_store = status;
        }
        ln_free(out);
        if (!loaded) {
            error = "The device key store is not available: " + nexus_user_key::store_failure_text(code);
            error_retryable = false;
            LOG(WARNING, kLogTag) << error << " (backend " << identity_store.value("backend", "unknown") << ")" << std::endl;
            store_retry_at = std::chrono::steady_clock::now() + store_retry_delay;
            store_retry_delay = std::min(store_retry_delay * 2, kRetryDelayMax);
            return;
        }
        store_retry_delay = kRetryDelayInitial;
        adopt_identity(std::move(loaded));
    }
    void require_identity() const {
        if (identity) return;
        if (source == IdentitySource::Handoff) {
            throw NexusError(409, "Waiting for the signed-in user's device key. Unlock Nexus from the Lemonade tray.");
        }
        throw NexusError(409, error.empty() ? "This device has no identity yet" : error);
    }
    void switch_account(const std::string& key) {
        const auto bound = account.value("public_key", std::string{});
        if (bound == key) return;
        if (bound.empty()) {
            // A file from before public_key was recorded belongs to an unknown key. The
            // federation's user_id tells whose it is at the first authentication.
            if (account.contains("user_id")) return;
            account["public_key"] = key;
            persist();
            return;
        }
        replace_account(nexus_access::stale_account_file(account), key);
    }
    void replace_account(const std::string& archive_name, const std::string& key) {
        const auto restore_path = root / ("account-" + nexus_access::account_file_tag(key) + ".json");
        json archived;
        if (std::ifstream input(restore_path); input) {
            archived = json::parse(input, nullptr, false);
            if (!archived.is_object()) throw NexusError(500, "Archived device account is invalid; restore " + restore_path.filename().string());
        }
        write_json(root / archive_name, account);
        account = nexus_access::account_for_key(account, archived, key);
        if (archived.is_object()) note_legacy_join();
        persist();
        std::error_code ec;
        std::filesystem::remove(restore_path, ec);
        LOG(INFO, kLogTag) << "Archived the account of another identity as " << archive_name << std::endl;
    }
    static std::string machine_name() {
        char buffer[256] = {};
        if (gethostname(buffer, sizeof(buffer) - 1) != 0 || buffer[0] == '\0') return "My Lemonade device";
        std::string name = buffer;
        static constexpr std::string_view local_suffix = ".local";
        if (name.size() > local_suffix.size() &&
            name.compare(name.size() - local_suffix.size(), local_suffix.size(), local_suffix) == 0) {
            name.resize(name.size() - local_suffix.size());
        }
        while (!name.empty() && name.back() == '.') name.pop_back();
        if (name.size() > kMaxNameBytes) name.resize(kMaxNameBytes);
        return name.empty() ? "My Lemonade device" : name;
    }
    std::string device_name() const {
        const auto named = account.value("device_name", std::string{});
        return named.empty() ? machine_name() : named;
    }
    int mesh_udp_port() const {
        const auto it = account.find("mesh_udp_port");
        return it != account.end() && it->is_number_integer() ? it->get<int>() : 0;
    }
    void persist() { write_json(root / kAccountFile, account); }
    static void write_json(const std::filesystem::path& path, const json& value) {
        auto temporary = path;
        temporary += ".tmp";
        std::ofstream output(temporary);
        output << value.dump(2);
        output.close();
        if (!output) throw NexusError(500, "Cannot save device account");
        std::error_code ec;
        if (!utils::atomic_replace_file(temporary, path, ec)) throw NexusError(500, "Cannot replace device account");
    }
    void lock() {
        locked = true;
        joined = false;
        reconnect = false;
        egress_ips.clear();
        ln_mesh_port_stop(client.get());
        ln_mesh_disable(client.get());
        ln_set_session_token(client.get(), "");
        ln_set_link_token(client.get(), "");
        devices = json::array();
        detail = SessionStatus::Locked;
        clear_error();
    }
    void clear_error() {
        error.clear();
        error_retryable = false;
    }
    void set_error(const std::exception& e) {
        error = e.what();
        const auto* failure = dynamic_cast<const NexusError*>(&e);
        error_retryable = failure != nullptr && failure->retryable;
    }
    void note_failure(const std::exception& e) {
        set_error(e);
        detail = SessionStatus::Degraded;
        note_refounded();
        if (const auto* failure = dynamic_cast<const NexusError*>(&e); failure && failure->identity_revoked) {
            replace_key("The network revoked this device's key: its account was deleted or the device was removed.");
        }
    }
    void replace_key(const std::string& why) {
        if (!identity) return;
        const auto revoked = pubkey;
        remove_local_account();
        remember_revoked_key(revoked);
        identity.reset();
        pubkey.clear();
        node_id.clear();
        group_id.clear();
        tunnel_ip.clear();
        LOG(WARNING, kLogTag) << "The network revoked device key " << revoked << "; the account files are removed and a new key replaces it" << std::endl;
        std::string next = source == IdentitySource::Handoff ? "Unlock Nexus from the Lemonade tray for a new device key, then join again."
                                                               : "A new device key is in use; join again with an invitation or create a network.";
        try {
            if (source == IdentitySource::System) load_system_identity();
            else if (source == IdentitySource::User) load_user_identity();  // the store replaces the revoked key
        } catch (const std::exception& e) { next = e.what(); }
        if (!identity && source == IdentitySource::User) next = error;  // the store's failure text
        if (why.empty()) return;
        error = why + " " + next;
        error_retryable = false;
    }
    // The revoked key outlives this session: the mesh is disabled after a deletion, and
    // a restart must not take the same key from the user store or the tray again.
    std::filesystem::path revoked_key_path() const { return root / "revoked-key.json"; }
    void remember_revoked_key(const std::string& key) {
        revoked_pubkey = key;
        write_json(revoked_key_path(), json{{"public_key", key}});
    }
    void load_revoked_key() {
        std::ifstream input(revoked_key_path());
        if (!input) return;
        const auto saved = json::parse(input, nullptr, false);
        if (saved.is_object()) revoked_pubkey = saved.value("public_key", std::string{});
    }
    void forget_revoked_key() {
        revoked_pubkey.clear();
        std::error_code ec;
        std::filesystem::remove(revoked_key_path(), ec);
    }
    // First connect without a configured key: the SDK takes the key only when
    // several network hosts agree on it over verified HTTPS, then it stays pinned.
    void ensure_genesis_key() {
        if (genesis_key_set) return;
        const auto found = sdk_call([&](auto out) { return ln_discover_genesis_key(client.get(), "[]", out); });
        account["genesis_pubkey"] = found.at("genesis_pubkey");
        persist();
        genesis_key_set = true;
    }
    json authenticate_identity() {
        require_identity();
        ensure_genesis_key();
        auto result = sdk_call([&](auto out) { return ln_auth_ed25519(client.get(), out); });
        const auto user_id = result.at("user_id").get<std::string>();
        switch (nexus_access::account_match(account, pubkey, user_id)) {
        case nexus_access::AccountMatch::Conflict: throw NexusError(403, "Account does not belong to this device identity");
        case nexus_access::AccountMatch::Stale: replace_account(nexus_access::stale_account_file(account), pubkey); break;
        case nexus_access::AccountMatch::Own: break;
        }
        account["user_id"] = user_id;
        if (account.value("public_key", std::string{}) != pubkey) {
            account["public_key"] = pubkey;
            persist();
        }
        return result;
    }
    bool key_unlock() const {
        return nexus_access::parse_unlock_method(account.value("unlock", std::string{})) == UnlockMethod::DeviceKey;
    }
    bool has_network() const { return account.value("joined", false); }
    // Accounts written before `joined` was recorded always joined on their first unlock.
    void note_legacy_join() {
        if (!account.contains("joined") && (key_unlock() || account.contains("credential_id"))) account["joined"] = true;
    }
    void unlock_with_key(bool defer_join, bool replace_passkey = false) {
        require_identity();
        const bool passkey = account.contains("credential_id");
        if (passkey && !replace_passkey) throw NexusError(409, "This device unlocks with its passkey");
        authenticate_identity();
        if (passkey) {
            account.erase("credential_id");
            LOG(INFO, kLogTag) << "Device key replaces the registered passkey of " << pubkey << std::endl;
        }
        if (passkey || !key_unlock()) {
            account["unlock"] = nexus_access::name(UnlockMethod::DeviceKey);
            persist();
        }
        hold_locked = false;
        locked = false;
        last_poll = std::chrono::system_clock::now();
        detail = joined ? SessionStatus::Running : SessionStatus::Ready;
        if (!defer_join && !joined) join("");
    }
    json private_call(const char* method, const std::string& path, const std::string& body = "") {
        auto value = sdk_call([&](auto out) { return ln_private_api_call(client.get(), method, path.c_str(), body.c_str(), out); });
        // The private API returns refusals as a JSON body rather than an SDK error code.
        if (value.is_object() && value.contains("error")) {
            const auto reason = text(value, "error");
            if (reason.rfind("no ", 0) == 0 && reason.find("permission") != std::string::npos) {
                throw NexusError(403, "This device does not have permission for that change");
            }
            if (reason.find("not found") != std::string::npos) throw NexusError(404, "That group or device no longer exists");
            const auto failure = nexus_access::sdk_failure(LN_ERR_REJECTED, value, 502);
            throw NexusError(failure.status, failure.message, failure.retryable);
        }
        return value;
    }

    json read_avatar() {
        const auto state = [](AvatarState value) { return json{{"state", nexus_access::name(value)}}; };
        if (!avatar_configured) return state(AvatarState::Unavailable);
        char* output = nullptr;
        const auto code = ln_account_avatar_get(client.get(), &output);
        std::unique_ptr<char, decltype(&ln_free)> owned(output, ln_free);
        if (code == LN_ERR_NOT_FOUND || code == LN_ERR_UNSUPPORTED) return state(AvatarState::Unsupported);
        if (code != LN_OK || !output) return state(AvatarState::Unavailable);
        try { return nexus_access::account_avatar(json::parse(output), group_id); }
        catch (const std::exception&) { return state(AvatarState::Unavailable); }
    }
    void set_assignment(const std::string& target, const std::string& key, const json& permissions) {
        const auto list = permissions.dump();
        char* output = nullptr;
        const auto code = permissions.empty()
            ? ln_remove_group_member(client.get(), target.c_str(), key.c_str(), &output)
            : ln_add_group_member(client.get(), target.c_str(), key.c_str(), list.c_str(), &output);
        std::unique_ptr<char, decltype(&ln_free)> owned(output, ln_free);
        if (code == LN_OK) return;
        const auto reply = output ? json::parse(output, nullptr, false) : json();
        const auto refusal = nexus_access::assignment_error(reply.is_object() ? text(reply, "error") : "");
        throw NexusError(refusal.first, refusal.second);
    }
    bool group_admin() const {
        const auto self = find_member(devices, node_id);
        return self.is_object() && self.value("is_admin", false);
    }
    json group_children() {
        auto children = private_call("GET", kTreeChildrenApi + group_id);
        if (!children.is_array()) throw NexusError(502, kMsgInvalidData);
        return children;
    }
    json scoped_tree_path(const std::string& id) {
        const auto fetch = [&](const std::string& current) { return private_call("GET", kTreeNodeApi + current); };
        try {
            return nexus_access::tree_path(id, group_id, fetch);
        } catch (const NexusError& e) {
            if (e.status != 403) throw;
        }
        // A node outside this device's group may sit under a group this identity entered
        // through an invite. The federation still checks every permission.
        std::vector<std::string> roots{group_id};
        const auto listed = memberships().value("memberships", json::array());
        for (const auto& membership : listed.is_array() ? listed : json::array()) {
            if (const auto root = text(membership, "node_id"); !root.empty()) roots.push_back(root);
        }
        return nexus_access::tree_path(id, roots, fetch);
    }
    json memberships() {
        auto result = sdk_call([&](auto out) { return ln_memberships_list(client.get(), out); }, 409);
        if (!result.is_object() || (result.contains("memberships") && !result["memberships"].is_array())) {
            throw NexusError(502, "The network returned invalid memberships");
        }
        return result;
    }
    json create_invite(const std::string& target, const json& body, const json& held, bool admin) {
        const auto ttl = body.value("ttl_sec", kInviteTtlMaxSeconds);
        if (ttl < kInviteTtlMinSeconds || ttl > kInviteTtlMaxSeconds) throw NexusError(400, "Invite lifetime must be 60 to 600 seconds");
        json requested = body.contains("permissions") ? body["permissions"] : json();
        if (body.contains("role")) {
            if (!requested.is_null()) throw NexusError(400, "Send a role or a list of permissions, not both");
            if (!body["role"].is_string()) throw NexusError(400, "Send role as a string");
            requested = nexus_access::role_permissions(role_from(body["role"].get<std::string>()));
        } else if (requested.is_null()) {
            // The federation's own default predates route families and would leave the
            // new device without svc:inference, so the user preset is the default here.
            auto defaults = nexus_access::default_invite_permissions(held, admin);
            if (!defaults.second.empty()) {
                LOG(INFO, kLogTag) << "Invite grants the user preset without " << defaults.second.dump()
                                   << ": this device does not hold them on the target group" << std::endl;
            }
            requested = std::move(defaults.first);
        }
        const auto permissions = nexus_access::invite_permissions(requested);
        const auto list = permissions.is_null() ? std::string() : permissions.dump();
        auto result = sdk_call([&](auto out) {
            return ln_invite_create(client.get(), target.empty() ? nullptr : target.c_str(),
                list.empty() ? nullptr : list.c_str(), static_cast<uint32_t>(ttl), out);
        }, 409);
        return result;
    }
    void remove_local_account() {
        lock();
        std::error_code ec;
        for (const auto* name : {kIdentityFile, kAccountFile, "account.json.tmp"}) std::filesystem::remove(root / name, ec);
        std::filesystem::remove_all(root / "avatar-cache", ec);
        std::filesystem::remove_all(root / "certs", ec);
        account = json::object();
    }
    json describe_tree_node(const json& path) {
        bool ancestor_admin = false;
        for (size_t i = 1; i < path.size(); ++i) {
            ancestor_admin = ancestor_admin || contains(permissions_for(path[i].value("assignments", json::array()), pubkey), Permission::Admin);
        }
        return nexus_access::tree_entry(path.front(), pubkey, ancestor_admin);
    }
    static bool group_entry(const json& entry) {
        return nexus_access::parse_tree_kind(text(entry, "kind")) == TreeKind::Group;
    }
    json tree_children(const std::string& id) {
        const auto path = scoped_tree_path(id);
        const auto parent = describe_tree_node(path);
        if (!group_entry(parent)) throw NexusError(400, "Only groups can contain other nodes");
        const auto children = private_call("GET", kTreeChildrenApi + id);
        if (!children.is_array()) throw NexusError(502, "The network returned invalid children");
        json entries = json::array();
        for (const auto& child : children) {
            if (text(child, "parent_id") != id || text(child, "id").empty()) throw NexusError(502, "The network returned an invalid hierarchy");
            auto child_path = path;
            child_path.insert(child_path.begin(), child);
            entries.push_back(describe_tree_node(child_path));
        }
        return {{"parent", parent}, {"children", entries}};
    }
    void refresh_devices() {
        const auto own = private_call("GET", kTreeNodeApi + node_id);
        group_id = own.at("parent_id").get<std::string>();
        const auto group = private_call("GET", kTreeNodeApi + group_id);
        const auto children = group_children();
        const auto peers = sdk_call([&](auto out) { return ln_mesh_peers(client.get(), out); });
        // Children leave out devices that joined this group from their own networks and
        // the devices of networks this identity joined; the SDK lists both. A failed list
        // stays null: the connect flags are then unknown, not false.
        json network = nullptr;
        try {
            network = sdk_call([&](auto out) { return ln_list_network_devices(client.get(), out); });
        } catch (const std::exception& e) {
            LOG(WARNING, kLogTag) << "Cannot list the network devices: " << e.what() << std::endl;
        }
        auto next = nexus_access::merge_members(group, children, peers, pubkey, node_id,
            mesh_status().value("peers", json::array()), network);
        for (auto it = egress_ips.begin(); it != egress_ips.end();) {
            const auto present = std::find_if(next.begin(), next.end(), [&](const json& device) {
                auto ip = device.value("tunnel_ip", "");
                if (const auto slash = ip.find('/'); slash != std::string::npos) ip.resize(slash);
                return device.value("node_id", "") == it->first && ip == it->second;
            });
            if (present == next.end()) {
                ln_mesh_close_egress(client.get(), it->second.c_str(), kMeshServicePort);
                it = egress_ips.erase(it);
            } else ++it;
        }
        devices = std::move(next);
    }
    uint16_t relay_port(const std::string& id) {
        if (locked) throw NexusError(423, kMsgUnlockFirst);
        if (!joined) throw NexusError(409, kMsgJoinFirst);
        const auto target = nexus_access::relay_target(devices, node_id, id);
        if (target.first != 0) throw NexusError(target.first, target.second);
        uint16_t port = 0;
        if (ln_mesh_open_egress(client.get(), target.second.c_str(), kMeshServicePort, &port) != LN_OK || !port) {
            throw NexusError(502, "Cannot open a mesh connection to that device");
        }
        egress_ips[id] = target.second;
        return port;
    }
    uint16_t reopen_relay_port(const std::string& id) {
        if (const auto known = egress_ips.find(id); known != egress_ips.end()) {
            ln_mesh_close_egress(client.get(), known->second.c_str(), kMeshServicePort);
            egress_ips.erase(known);
        }
        return relay_port(id);
    }
    json mesh_status() {
        try { return sdk_call([&](auto out) { return ln_mesh_status(client.get(), out); }); }
        catch (const std::exception&) { return json::object(); }
    }
    json status() {
        auto mesh = mesh_status();
        json result = {{"status", nexus_access::name(detail)}, {"locked", locked}, {"mesh_up", !locked && ln_mesh_is_active(client.get()) != 0},
            {"node_id", node_id}, {"group_node_id", group_id}, {"tunnel_ip", tunnel_ip},
            {"peer_count", mesh.value("peer_count", 0)}, {"tunnel_mtu", mesh.value("tunnel_mtu", 0)},
            {"mesh_udp_port", mesh_udp_port()}, {"dataplane_port", mesh.value("dataplane_port", 0)},
            {"mapped_endpoint", mesh.value("mapped_endpoint", "")}, {"mapping_protocol", mesh.value("mapping_protocol", "")},
            {"device_name", device_name()}, {"hostname", machine_name()}, {"server", host},
            {"registered", account.contains("credential_id") || key_unlock()}, {"has_network", has_network()},
            {"unlock", nexus_access::name(key_unlock() ? UnlockMethod::DeviceKey : UnlockMethod::Passkey)}, {"members", devices},
            {"exposed", json::array({{{"vport", kMeshServicePort}, {"local_port", ingress_port}}})}, {"error", error},
            {"error_retryable", error_retryable}, {"genesis_key_set", genesis_key_set}, {"mesh_port", mesh_port_status()},
            {"catalog_error", catalog_error}, {"identity_source", nexus_access::identity_source_name(source)},
            {"needs_user_key", !identity}, {"public_key", pubkey}, {"needs_rejoin", refounded},
            {"revoked_public_key", revoked_pubkey}};
        if (source == IdentitySource::User) result["identity_store"] = identity_store;
        if (!join_membership.is_null()) result["join_membership"] = join_membership;
        return result;
    }
    json begin_handoff() {
        char* out = nullptr;
        handoff.reset(ln_identity_handoff_begin(&out));
        std::unique_ptr<char, decltype(&ln_free)> owned(out, ln_free);
        const auto request = out ? json::parse(out, nullptr, false) : json();
        if (!handoff || !request.is_object()) throw NexusError(500, "Cannot start an identity hand-off");
        return request;
    }
    void open_handoff(const json& envelope) {
        if (!envelope.is_string()) throw NexusError(400, "Send identity_envelope as a string");
        if (!handoff) throw NexusError(409, "No identity hand-off is outstanding. Request key/handoff first.");
        ln_error_t code = LN_OK;
        IdentityPtr received(ln_identity_handoff_open(handoff.get(), envelope.get_ref<const std::string&>().c_str(), &code), ln_identity_destroy);
        // The SDK wipes the ephemeral secret on the first open whatever the result.
        handoff.reset();
        if (!received) {
            const auto failure = nexus_access::handoff_failure(code);
            throw NexusError(failure.first, failure.second);
        }
        adopt_identity(std::move(received));
        if (!account.contains("credential_id") && !key_unlock()) {
            account["unlock"] = nexus_access::name(UnlockMethod::DeviceKey);
            persist();
        }
    }
    json mesh_port_status() {
        json port = sdk_call([&](auto out) { return ln_mesh_port_status(client.get(), out); });
        if (!mesh_port_error.empty()) port["error"] = mesh_port_error;
        return port;
    }
    // The SDK keeps the catalog with the mesh session, so every join registers it
    // again, before the service is exposed: without a catalog the SDK would bridge
    // the port raw.
    bool register_catalog() {
        catalog_error.clear();
        if (!catalog.is_object()) {
            catalog_error = "lemond has no route catalog";
            LOG(WARNING, kLogTag) << catalog_error << std::endl;
            return false;
        }
        const auto text = catalog.dump();
        const auto code = ln_mesh_service_set_catalog(client.get(), kMeshServicePort, text.c_str());
        if (code != LN_OK) {
            catalog_error = "The SDK refused the route catalog (SDK error " + std::to_string(code) + ")";
            LOG(WARNING, kLogTag) << catalog_error << std::endl;
            return false;
        }
        LOG(INFO, kLogTag) << "Registered route catalog version " << catalog.value("version", uint64_t{0})
                           << " with " << catalog.value("families", nlohmann::ordered_json::array()).size()
                           << " families" << std::endl;
        return true;
    }
    void start_mesh_port() {
        mesh_port_error.clear();
        const auto cert_dir = utils::path_to_utf8(root / "certs" / "group-loopback");
        std::error_code ec;
        std::filesystem::create_directories(root / "certs" / "group-loopback", ec);
        ln_mesh_port_set_cert_dir(client.get(), cert_dir.c_str());
        const auto local = nexus_user_key::loopback_url(local_service_port);
        const auto started = ln_mesh_port_start(client.get(), static_cast<uint16_t>(mesh_port), local.c_str());
        if (started == LN_ERR_PORT_IN_USE) {
            mesh_port_error = "Another program, such as the Lemonade Nexus app, already uses the mesh port";
        } else if (started != LN_OK) {
            mesh_port_error = "The mesh port could not start (SDK error " + std::to_string(started) + ")";
        }
    }
    json selection(const std::string& method, const json& body) {
        if (method == "POST") {
            const auto value = body.contains("target") ? body["target"] : json(nullptr);
            if (!value.is_null() && !value.is_string()) throw NexusError(400, "Send target as \"local\", \"auto\", a node ID or null");
            if (body.contains("peer_key") && !body["peer_key"].is_string()) throw NexusError(400, "Send peer_key as a string");
            const auto target = value.is_string() ? value.get<std::string>() : std::string();
            const auto key = body.value("peer_key", std::string());
            if (!nexus_access::selection_key_allowed(target, key)) {
                throw NexusError(400, "Auto routing cannot carry a peer_key: select the device the key belongs to");
            }
            const auto code = ln_mesh_port_set_selection(client.get(), target.empty() ? nullptr : target.c_str(),
                key.empty() ? nullptr : key.c_str());
            if (code == LN_ERR_NOT_FOUND) throw NexusError(404, kMsgNotInGroup);
            if (code == LN_ERR_AUTH) throw NexusError(403, "This device may not connect to that device");
            if (code == LN_ERR_REJECTED) throw NexusError(409, "The mesh port is not running, or that target cannot be selected");
            if (code != LN_OK) throw NexusError(502, "The mesh port refused the selection");
        }
        const auto port = mesh_port_status();
        auto reply = port.value("selection", json::object());
        reply["port"] = port.value("port", 0);
        reply["running"] = port.value("running", false);
        return reply;
    }
    void join(const std::string& code) {
        if (locked) throw NexusError(423, "Unlock this device before joining");
        if (ingress_port <= 0) throw NexusError(503, "The mesh service listener is not running");
        detail = SessionStatus::Joining;
        ln_set_link_token(client.get(), code.c_str());
        if (ln_set_inference_node(client.get(), 1) != LN_OK) throw NexusError(500, "Cannot join as a Lemonade server");
        // The join names the device; a name the SDK refuses (too long, or with a
        // character peers would drop) leaves the server default, renamed below.
        ln_set_device_name(client.get(), device_name().c_str());
        json joined_result;
        try { joined_result = sdk_call([&](auto out) { return ln_join_network(client.get(), device_name().c_str(), "", out); }); }
        catch (const std::exception& e) {
            ln_set_link_token(client.get(), "");
            note_failure(e);
            throw;
        }
        ln_set_link_token(client.get(), "");
        node_id = joined_result.at("node_id").get<std::string>();
        tunnel_ip = joined_result.at("tunnel_ip").get<std::string>();
        // A code used by a device that already has a node does not move it: the device
        // stays in its own network and the code makes it a member of the code's group.
        join_membership = joined_result.value("membership", json(nullptr));
        if (!register_catalog()) {
            lock();
            throw NexusError(502, catalog_error);
        }
        start_mesh_transport();
        joined = true;
        reconnect = true;
        retry_delay = kRetryDelayInitial;
        detail = SessionStatus::Running;
        clear_error();
        if (!account.value("joined", false)) {
            account["joined"] = true;
            persist();
        }
        start_mesh_port();
        remember_tier1_epoch();
        try {
            refresh_devices();
            // The join names the node it creates. A returning device renamed while
            // it was away still needs the write; it commits through Tier 1
            // consensus, so an unchanged name sends none.
            const auto self = find_member(devices, node_id);
            if (!self.is_object() || self.value("device_name", std::string{}) != device_name()) {
                const auto updates = json{{"hostname", device_name()}}.dump();
                sdk_call([&](auto out) { return ln_update_node(client.get(), node_id.c_str(), updates.c_str(), out); });
                refresh_devices();
            }
        } catch (const std::exception& e) { set_error(e); }
    }
    // A fixed mesh_udp_port binds exclusively, so a router mapping or a manual port
    // forward to it stays valid; the SDK moves the dataplane when the port differs
    // from the one in use.
    void start_mesh_transport() {
        const auto port = mesh_udp_port();
        const auto config = json{{"listen_port", port}}.dump();
        const auto service = "tcp:127.0.0.1:" + std::to_string(ingress_port);
        const bool started = (port == 0 ? ln_mesh_enable(client.get()) : ln_mesh_enable_config(client.get(), config.c_str())) == LN_OK &&
            ln_mesh_expose_service(client.get(), kMeshServicePort, service.c_str()) == LN_OK;
        const auto failure = nexus_access::mesh_transport_failure(started, port, mesh_status());
        if (failure.empty()) return;
        lock();
        error = failure;
        throw NexusError(502, failure);
    }
    // The lowest epoch the SDK accepts, for the chain the device last verified. A floor
    // stored before the chain was recorded (an earlier release) could belong to a network
    // that was since founded again, so it is dropped rather than applied to any chain.
    void apply_epoch_floor() {
        const auto chain = account.value("tier1_chain", std::string{});
        const auto epoch = account.value("tier1_epoch", uint64_t{0});
        if (!chain.empty() && epoch != 0 && ln_set_epoch_floor(client.get(), chain.c_str(), epoch) == LN_OK) return;
        if (account.contains("tier1_epoch") || account.contains("tier1_chain")) {
            account.erase("tier1_epoch");
            account.erase("tier1_chain");
            persist();
        }
    }
    // A membership of another chain under the pinned key means the network was founded
    // again: the SDK neither follows it nor applies the floor, and status reports
    // needs_rejoin until the user forgets this network and joins it again.
    void remember_tier1_epoch() {
        try {
            const auto membership = sdk_call([&](auto out) { return ln_tier1_membership(client.get(), out); });
            refounded = membership.value("refounded", false);
            if (refounded) return;
            const auto epoch = nexus_access::newer_tier1_epoch(membership, account.value("tier1_epoch", uint64_t{0}));
            const auto chain = membership.value("chain", std::string{});
            if (epoch != 0 && !chain.empty()) {
                account["tier1_epoch"] = epoch;
                account["tier1_chain"] = chain;
                persist();
            }
        } catch (const std::exception&) {}
    }
    void note_refounded() {
        try {
            const auto membership = sdk_call([&](auto out) { return ln_tier1_membership(client.get(), out); });
            refounded = membership.value("refounded", false);
        } catch (const std::exception&) {}
    }
    void poll() {
        const auto now = std::chrono::system_clock::now();
        const auto elapsed = now - last_poll;
        last_poll = now;
        if (!locked && elapsed > kIdleLockAfter) lock();
        if (!identity) {
            if (source == IdentitySource::User && std::chrono::steady_clock::now() >= store_retry_at) load_user_identity();
            if (!identity) return;
        }
        if (locked) {
            if (!key_unlock() || !has_network() || hold_locked || std::chrono::steady_clock::now() < retry_at) return;
            try { unlock_with_key(false); }
            catch (const std::exception& e) {
                note_failure(e);
                retry_at = std::chrono::steady_clock::now() + retry_delay;
                retry_delay = std::min(retry_delay * 2, kRetryDelayMax);
            }
            return;
        }
        if (!reconnect || std::chrono::steady_clock::now() < retry_at) return;
        try {
            // The SDK binds a session to the Tier 1 server that issued it and drops the
            // session when that server leaves Tier 1; the mesh can stay up while every
            // private call is refused, so the flag decides, not the mesh state.
            if (!joined || !ln_mesh_is_active(client.get()) || nexus_access::session_dropped(mesh_status())) {
                ln_mesh_disable(client.get());
                ln_set_session_token(client.get(), "");
                egress_ips.clear();
                joined = false;
                join("");
            }
            else {
                refresh_devices();
                remember_tier1_epoch();
                clear_error();
                detail = SessionStatus::Running;
            }
        } catch (const std::exception& e) {
            note_failure(e);
            if (nexus_access::session_dropped(mesh_status())) {
                ln_mesh_disable(client.get());
                ln_set_session_token(client.get(), "");
                egress_ips.clear();
                joined = false;
                retry_at = std::chrono::steady_clock::now() + kSessionDropRetryDelay;
                return;
            }
            retry_at = std::chrono::steady_clock::now() + retry_delay;
            retry_delay = std::min(retry_delay * 2, kRetryDelayMax);
        }
    }
    static bool tree_node_operation(Operation operation) {
        switch (operation) {
        case Operation::TreeChildren: case Operation::TreeGroups: case Operation::TreeRename: case Operation::TreePermissions:
        case Operation::TreeInvites: case Operation::TreeMove:
            return true;
        default:
            return false;
        }
    }
    json request(Operation operation, const std::string& method, const std::string& id, const json& body, const std::string& caller_key) {
        try {
            if (!caller_key.empty()) {
                if (locked) throw NexusError(423, kMsgUnlockFirst);
                const auto require_admin = [&](const std::string& node) {
                    if (!nexus_access::administers_tree(scoped_tree_path(node), caller_key)) {
                        throw NexusError(403, "This Nexus caller does not administer the requested group");
                    }
                };
                require_admin(group_id);
                if (tree_node_operation(operation)) {
                    require_admin(id);
                    if (operation == Operation::TreeMove) require_admin(body.at("parent_id").get<std::string>());
                }
            }
            return dispatch(operation, method, id, body);
        }
        catch (const NexusError& e) {
            // A federation that cannot be reached is not a refusal: the device stays
            // unlocked and joined, and status reports the outage until the next success.
            if (e.retryable && !locked) note_failure(e);
            throw;
        }
    }
    json dispatch(Operation operation, const std::string& method, const std::string& id, const json& body) {
        if (operation == Operation::Status) return status();
        if (operation == Operation::Selection) return selection(method, body);
        if (operation == Operation::Device) {
            if (!body.contains("device_name") && !body.contains("mesh_udp_port")) throw NexusError(400, "Send device_name or mesh_udp_port");
            if (body.contains("device_name")) {
                const auto name = body.at("device_name").get<std::string>();
                if (name.empty() || name.size() > kMaxNameBytes) throw NexusError(400, kMsgDeviceName);
                if (joined && !locked) {
                    const auto updates = json{{"hostname", name}}.dump();
                    sdk_call([&](auto out) { return ln_update_node(client.get(), node_id.c_str(), updates.c_str(), out); });
                }
                account["device_name"] = name;
                persist();
            }
            if (body.contains("mesh_udp_port")) {
                const auto& value = body.at("mesh_udp_port");
                if (!value.is_number_integer() || value.get<int64_t>() < 0 || value.get<int64_t>() > kMaxPort) {
                    throw NexusError(400, "Enter a UDP port (1-65535), or 0 for any port");
                }
                const auto port = value.get<int>();
                if (port != mesh_udp_port()) {
                    account["mesh_udp_port"] = port;
                    persist();
                    if (joined && !locked) start_mesh_transport();
                }
            }
            return status();
        }
        if (operation == Operation::PasskeyChallenge) {
            const auto authenticated = authenticate_identity();
            const auto user_id = authenticated.at("user_id").get<std::string>();
            const auto challenge = sdk_call([&](auto out) { return ln_auth_passkey_challenge(client.get(), user_id.c_str(), out); });
            if (rp_id.empty()) {
                // The RP ID is the network's, not this host's: WebAuthn binds the credential to it.
                const auto health = sdk_call([&](auto out) { return ln_health(client.get(), out); });
                rp_id = health.value("rp_id", std::string{});
                if (rp_id.empty()) throw NexusError(502, "The network did not report its passkey RP ID");
            }
            return {{"challenge", challenge.at("challenge")}, {"user_id", user_id}, {"rp_id", rp_id},
                {"credential_id", account.value("credential_id", "")}};
        }
        if (operation == Operation::PasskeyRegister) {
            if (account.contains("credential_id")) throw NexusError(409, "This device already has a passkey");
            if (key_unlock()) throw NexusError(409, "This device unlocks with its device key");
            if (!account.contains("user_id")) throw NexusError(409, "Request a registration challenge first");
            const auto authenticated = authenticate_identity();
            const auto user_id = authenticated.at("user_id").get<std::string>();
            const auto credential_id = body.at("credential_id").get<std::string>();
            const auto x = body.at("public_key_x").get<std::string>();
            const auto y = body.at("public_key_y").get<std::string>();
            sdk_call([&](auto out) { return ln_register_passkey(client.get(), user_id.c_str(), credential_id.c_str(), x.c_str(), y.c_str(), out); });
            account["credential_id"] = credential_id;
            persist();
            return status();
        }
        if (operation == Operation::KeyHandoff) return begin_handoff();
        if (operation == Operation::KeyUnlock) {
            if (body.contains("identity_envelope")) open_handoff(body.at("identity_envelope"));
            unlock_with_key(body.value("defer_join", !has_network()), body.value("replace_passkey", false));
            return status();
        }
        if (operation == Operation::PasskeyUnlock) {
            if (key_unlock()) throw NexusError(409, "This device unlocks with its device key");
            const auto& assertion = body.at("passkey_assertion");
            if (!account.contains("credential_id") || assertion.at("credential_id") != account.at("credential_id")) throw NexusError(403, "Use this device's registered passkey");
            auto encoded = assertion.at("authenticator_data").get<std::string>();
            std::replace(encoded.begin(), encoded.end(), '-', '+');
            std::replace(encoded.begin(), encoded.end(), '_', '/');
            while (encoded.size() % 4) encoded += '=';
            const auto bytes = utils::JsonUtils::base64_decode(encoded);
            if (bytes.size() < kAuthenticatorDataMinBytes ||
                (static_cast<unsigned char>(bytes[kAuthenticatorFlagsOffset]) & kUserPresentAndVerified) != kUserPresentAndVerified) {
                throw NexusError(403, "Passkey user verification is required");
            }
            const auto payload = json{{"assertion", assertion}}.dump();
            const auto authenticated = sdk_call([&](auto out) { return ln_auth_passkey(client.get(), payload.c_str(), out); });
            if (authenticated.at("user_id") != account.at("user_id")) {
                ln_set_session_token(client.get(), "");
                throw NexusError(403, "Passkey does not match this device identity");
            }
            authenticate_identity();
            locked = false;
            last_poll = std::chrono::system_clock::now();
            detail = joined ? SessionStatus::Running : SessionStatus::Ready;
            if (!body.value("defer_join", false) && !joined) join("");
            return status();
        }
        if (operation == Operation::Lock) { hold_locked = true; lock(); return status(); }
        if (operation == Operation::Forget) {
            if (!refounded) throw NexusError(409, "This network was not founded again; use Leave network or Delete account instead");
            ln_mesh_disable(client.get());
            ln_set_session_token(client.get(), "");
            egress_ips.clear();
            joined = false;
            remove_local_account();
            refounded = false;
            return {{"forgotten", true}};
        }
        if (operation == Operation::InvitesCancel) return {{"cancelled", true}, {"revoked", false}};
        if (locked) throw NexusError(423, kMsgUnlockFirst);
        if (operation == Operation::DeviceCheck) {
            // Advisory: the federation still refuses a collision on join or rename, so
            // two devices checking the same name at once cannot both win.
            const auto name = body.contains("name") ? body.value("name", std::string{}) : body.value("device_name", std::string{});
            if (name.empty() || name.size() > kMaxNameBytes) throw NexusError(400, kMsgDeviceName);
            const auto code = body.value("code", std::string{});
            if (joined && code.empty()) {
                return sdk_call([&](auto out) { return ln_check_device_name(client.get(), name.c_str(), group_id.c_str(), "", out); });
            }
            if (!nexus_access::invite_code(code)) throw NexusError(400, "Enter the invite code for the group you are joining");
            return sdk_call([&](auto out) { return ln_check_device_name(client.get(), name.c_str(), "", code.c_str(), out); });
        }
        if (operation == Operation::Join) {
            const auto code = body.at("code").get<std::string>();
            if (!nexus_access::invite_code(code)) throw NexusError(400, kMsgInviteCode);
            if (joined) throw NexusError(409, "This device already joined. Accept the invite to join another network.");
            join(code);
            return status();
        }
        if (!joined) throw NexusError(409, kMsgJoinFirst);
        if (operation == Operation::InvitesAccept) {
            const auto code = body.at("code").get<std::string>();
            if (!nexus_access::invite_code(code)) throw NexusError(400, kMsgInviteCode);
            auto result = sdk_call([&](auto out) { return ln_invite_accept(client.get(), code.c_str(), out); }, 409);
            refresh_devices();
            return result;
        }
        if (operation == Operation::Memberships) {
            auto result = memberships();
            result["group_node_id"] = group_id;
            return result;
        }
        if (operation == Operation::MembershipsLeave) {
            if (id == group_id) throw NexusError(403, "This device's own group cannot be left. Remove the device instead.");
            sdk_call([&](auto out) { return ln_membership_leave(client.get(), id.c_str(), out); }, 409);
            return {{"left", true}, {"node_id", id}};
        }
        if (operation == Operation::AccountDelete) {
            if (body.value("account_id", "") != group_id || body.value("member_id", "") != account.value("user_id", "") ||
                body.value("device_node_id", "") != node_id) throw NexusError(409, kMsgAccountChanged);
            if (body.value("confirm", "") != "delete") throw NexusError(400, "Send \"confirm\": \"delete\" to delete this account permanently");
            auto result = sdk_call([&](auto out) { return ln_account_delete(client.get(), out); }, 409);
            if (text(result, "status") == "deleted") replace_key("");
            return result;
        }
        if (operation == Operation::Account) {
            refresh_devices();
            auto result = nexus_access::account_summary(devices, group_id, node_id, account.value("user_id", ""));
            result["devices"] = devices;
            result["avatar"] = read_avatar();
            return result;
        }
        if (operation == Operation::AccountAvatar) {
            if (body.value("account_id", "") != group_id || body.value("member_id", "") != account.value("user_id", "") ||
                body.value("device_node_id", "") != node_id) throw NexusError(409, kMsgAccountChanged);
            const auto current = read_avatar();
            const auto state = nexus_access::parse_avatar_state(text(current, "state"));
            if (state == AvatarState::Unsupported) throw NexusError(501, "This mesh needs a Nexus server update to sync profile images.");
            if (state != AvatarState::Ready) throw NexusError(502, "Cannot reach the account image service. Try again.");
            if (!current.at("can_write").get<bool>()) throw NexusError(403, "You do not have permission to change this account image.");
            if (body.at("revision") != current.at("revision")) throw NexusError(409, "The profile image changed. Refresh and try again.");
            const auto revision = current.at("revision").get<uint64_t>();
            if (revision >= kMaxSafeInteger) throw NexusError(409, "The account image revision limit was reached.");
            const auto& image = body.at("image");
            if (!image.is_null() && !image.is_string()) throw NexusError(400, "Expected a profile image or null");
            const auto data = image.is_null() ? std::string() : image.get<std::string>();
            if (data.size() > kMaxAvatarImageBytes || (!data.empty() && data.rfind(kPngDataUrlPrefix, 0) != 0)) throw NexusError(400, "Choose a PNG profile image under 128 KiB.");
            const auto record = sdk_call([&](auto out) { return ln_account_avatar_sign(identity.get(), group_id.c_str(), data.c_str(), revision + 1, out); });
            char* output = nullptr;
            const auto encoded = record.dump();
            const auto code = ln_account_avatar_put(client.get(), encoded.c_str(), data.c_str(), &output);
            std::unique_ptr<char, decltype(&ln_free)> owned(output, ln_free);
            const auto result = output ? json::parse(output, nullptr, false) : json();
            if (code != LN_OK) {
                const auto message = result.is_object() ? result.value("error", kMsgAvatarNotSaved) : kMsgAvatarNotSaved;
                throw NexusError(code == LN_ERR_AUTH ? 403 : code == LN_ERR_REJECTED ? 409 : code == LN_ERR_NOT_FOUND ? 501 : 502, message);
            }
            return nexus_access::account_avatar(result, group_id);
        }
        if (operation == Operation::Tree) return tree_children(group_id);
        if (operation == Operation::TreeChildren) return tree_children(id);
        if (tree_node_operation(operation)) {
            const auto chain = scoped_tree_path(id);
            const auto target = describe_tree_node(chain);
            if (operation == Operation::TreeMove) {
                const auto parent_id = body.at("parent_id").get<std::string>();
                if (parent_id == id) throw NexusError(400, "A node cannot move under itself");
                const auto destination = describe_tree_node(scoped_tree_path(parent_id));
                if (!group_entry(destination)) throw NexusError(400, "Move a device or group into a group");
                if (!destination.at("can_add_group").get<bool>()) throw NexusError(403, "You do not have permission to add to that group");
                auto result = sdk_call([&](auto out) { return ln_move_node(client.get(), id.c_str(), parent_id.c_str(), out); }, 409);
                refresh_devices();
                return result;
            }
            if (!group_entry(target)) throw NexusError(400, "Select a group for this action");
            if (operation == Operation::TreeInvites) {
                if (!target.at("can_add_group").get<bool>()) throw NexusError(403, "You do not have permission to invite members to this group");
                const auto name = body.value("device_name", std::string());
                if (name.size() > kMaxNameBytes) throw NexusError(400, kMsgName);
                auto result = create_invite(id, body, target.at("permissions"), target.at("is_admin").get<bool>());
                if (!name.empty()) result["device_name"] = name;
                return result;
            }
            if (operation == Operation::TreeGroups || operation == Operation::TreeRename) {
                const bool create = operation == Operation::TreeGroups;
                if (!target.at(create ? "can_add_group" : "can_rename").get<bool>()) throw NexusError(403, "You do not have permission to change this group");
                const auto name = body.at("name").get<std::string>();
                if (name.empty() || name.size() > kMaxNameBytes || name.find_first_not_of(" \t\r\n") == std::string::npos) throw NexusError(400, kMsgGroupName);
                const auto result = create
                    ? private_call("POST", "/api/tree/node", json{{"parent_id", id}, {"type", nexus_access::name(NodeType::Customer)}, {"hostname", name}}.dump())
                    : private_call("POST", std::string(kTreeNodeApi) + "update/" + id, json{{"hostname", name}}.dump());
                if (!result.value("success", false)) throw NexusError(502, "The network could not save the group");
                return result;
            }
            if (!target.at("can_manage").get<bool>()) throw NexusError(403, "Only an admin with access to this group's assignments can change permissions");
            const auto member = scoped_tree_path(body.at("member_node_id").get<std::string>()).front();
            if (nexus_access::parse_node_type(text(member, "type")) != NodeType::Endpoint) throw NexusError(400, "Choose a device node");
            const auto key = text(member, "mgmt_pubkey");
            if (key.empty() || nexus_access::same_key(key, pubkey)) throw NexusError(403, kMsgOwnAccess);
            const bool admin = body.at("admin").get<bool>();
            if (!admin && nexus_access::same_key(key, text(chain.front(), "mgmt_pubkey"))) throw NexusError(403, kMsgCreatorAdministers);
            const auto permissions = nexus_access::group_permissions(body.at("permissions"), admin);
            set_assignment(id, key, permissions);
            return tree_children(id);
        }
        if (operation == Operation::Devices) { refresh_devices(); return devices; }
        if (operation == Operation::Members) {
            refresh_devices();
            return {{"group_node_id", group_id}, {"can_manage", group_admin()}, {"members", devices}};
        }
        if (operation == Operation::Invites) {
            const auto name = body.value("device_name", std::string());
            if (name.size() > kMaxNameBytes) throw NexusError(400, kMsgName);
            const auto self = find_member(devices, node_id);
            auto result = create_invite("", body, self.is_object() ? self.value("permissions", json::array()) : json::array(),
                self.is_object() && self.value("is_admin", false));
            if (!name.empty()) result["device_name"] = name;
            return result;
        }
        refresh_devices();
        if (operation == Operation::Egress) {
            const auto peer_id = body.at("node_id").get<std::string>();
            const auto target = find_member(devices, peer_id);
            if (target.is_null() || peer_id == node_id) throw NexusError(404, kMsgChooseAnotherMember);
            auto ip = target.at("tunnel_ip").get<std::string>();
            if (auto slash = ip.find('/'); slash != std::string::npos) ip.resize(slash);
            uint16_t port = 0;
            if (ln_mesh_open_egress(client.get(), ip.c_str(), kMeshServicePort, &port) != LN_OK || !port) throw NexusError(502, "Remote server connection failed");
            egress_ips[peer_id] = ip;
            return {{"node_id", peer_id}, {"loopback_port", port}, {"endpoint", nexus_user_key::loopback_url(port)}};
        }
        if (id.empty()) throw NexusError(404, kMsgUnknownNetwork);
        const auto target = find_member(devices, id);
        if (target.is_null()) throw NexusError(404, kMsgNotInGroup);
        if (operation == Operation::DevicesRemove) {
            if (id == node_id) throw NexusError(404, kMsgChooseAnotherMember);
            if (!target.at("can_remove").get<bool>()) throw NexusError(403, "This device does not have permission to remove that device");
            const auto removed_key = target.at("management_pubkey").get<std::string>();
            private_call("POST", "/api/tree/node/delete/" + id, "{}");
            if (group_admin()) set_assignment(group_id, removed_key, json::array());
            refresh_devices();
            return {{"removed", true}};
        }
        if (operation == Operation::DevicesAccess) {
            if (method == "POST") {
                if (!group_admin()) throw NexusError(403, "Only a group admin can change device access");
                if (!target.at("access_known").get<bool>()) {
                    throw NexusError(409, "This device's access list is not visible to you. Ask the admin who invited it.");
                }
                const auto member_id = body.at("member_node_id").get<std::string>();
                const bool connect = body.at("connect").get<bool>();
                const bool see = body.at("see").get<bool>() || connect;
                if (member_id == node_id) throw NexusError(403, kMsgOwnAccess);
                const auto member = find_member(devices, member_id);
                if (member.is_null() || member_id == id) throw NexusError(404, kMsgChooseAnotherMember);
                const auto key = member.at("management_pubkey").get<std::string>();
                const auto current = permissions_for(target.at("access"), key);
                if (contains(current, Permission::Admin)) throw NexusError(409, "That member administers this device; change its role instead");
                set_assignment(id, key, nexus_access::device_permissions(current, see, connect));
                refresh_devices();
            }
            return nexus_access::device_access(devices, id, node_id);
        }
        if (operation == Operation::MembersRole || operation == Operation::MembersPermissions) {
            std::optional<Role> role;
            if (operation == Operation::MembersRole) role = role_from(body.at("role").get<std::string>());
            if (!group_admin()) throw NexusError(403, kMsgOnlyGroupAdmin);
            if (id == node_id) throw NexusError(403, role ? "You cannot change your own role. Ask another admin." : "You cannot change your own access. Ask another admin.");
            const auto key = target.at("management_pubkey").get<std::string>();
            const bool admin = target.at("is_admin").get<bool>();
            json permissions;
            if (role) {
                permissions = nexus_access::role_permissions(*role);
                if (!contains(permissions, Permission::Admin) && target.at("is_owner").get<bool>()) throw NexusError(403, kMsgCreatorAdministers);
            } else {
                permissions = nexus_access::group_permissions(body.at("permissions"), admin);
            }
            set_assignment(group_id, key, permissions);
            refresh_devices();
            return find_member(devices, id);
        }
        throw NexusError(404, kMsgUnknownNetwork);
    }
};

namespace {
// Runs on SDK threads while the SDK holds locks: copy the text, hand it to the server
// log, return. No ln_* call, no exception out.
void sdk_log_line(int level, const char*, const char* message, void*) noexcept {
    try {
        const std::string text = message ? message : "";
        switch (level) {
        case 0: LOG(TRACE, kSdkLogTag) << text << std::endl; break;
        case 1: LOG(DEBUG, kSdkLogTag) << text << std::endl; break;
        case 2: LOG(INFO, kSdkLogTag) << text << std::endl; break;
        case 3: LOG(WARNING, kSdkLogTag) << text << std::endl; break;
        default: LOG(ERROR, kSdkLogTag) << text << std::endl; break;
        }
    } catch (...) {
    }
}
// Routes the SDK's log to the server log once per process, before the first ln_create:
// LemonadeServer.exe has no console, so lines left on stdout are lost there.
void route_sdk_log() {
    static const bool routed = [] {
        if (ln_set_log_callback(&sdk_log_line, nullptr) == LN_OK) return true;
        LOG(WARNING, kLogTag) << "Cannot route the Nexus SDK log to the server log; it stays on stdout" << std::endl;
        return false;
    }();
    (void)routed;
}
}

NexusManager::NexusManager(const std::string& config_dir,
                           const std::function<void(httplib::Server&)>& install_policy,
                           const std::function<void(httplib::Server&)>& install_api,
                           int mesh_port,
                           nlohmann::ordered_json service_catalog,
                           std::optional<ProcessRole> process_role,
                           const std::function<void(httplib::Server&)>& install_local_api,
                           json route_permissions)
    : data_root_(utils::path_to_utf8(utils::path_from_utf8(config_dir) / "nexus")), catalog_(std::move(service_catalog)),
      route_permissions_(std::move(route_permissions)),
      role_(process_role ? *process_role : nexus_access::current_process_role()), selection_port_(mesh_port) {
    route_sdk_log();
    if (!catalog_.is_object()) {
        LOG(INFO, kLogTag) << "Route policy is unavailable: no route catalog" << std::endl;
    } else if (!nexus_access::route_policy_available()) {
        LOG(INFO, kLogTag) << "Route policy is unavailable: the Nexus SDK does not export ln_mesh_service_set_catalog" << std::endl;
    }
    // Built once and never rebound: a main-port rebind leaves the mesh listener alone.
    register_routes(mesh_server_, NexusListener::MeshIngress);
    install_api(mesh_server_);
    mesh_server_.set_read_timeout(kMeshListenerReadTimeoutSeconds, 0);
    mesh_server_.set_write_timeout(kMeshListenerWriteTimeoutSeconds, 0);
    ingress_port_ = std::max(mesh_server_.bind_to_any_port("127.0.0.1"), 0);
    if (ingress_port_ > 0) {
        mesh_thread_ = std::thread([this] { mesh_server_.listen_after_bind(); });
        mesh_server_.wait_until_ready();
    } else {
        LOG(WARNING, kLogTag) << "Cannot bind the mesh listener; mesh peers cannot reach this server" << std::endl;
    }
    register_routes(management_server_, NexusListener::Management);
    if (install_local_api) install_local_api(management_server_);
    install_policy(management_server_);
    management_port_ = management_server_.bind_to_any_port("127.0.0.1");
    if (management_port_ < 0) { stop(); throw std::runtime_error("Cannot bind local network management listener"); }
    management_thread_ = std::thread([this] { management_server_.listen_after_bind(); });
    management_server_.wait_until_ready();
    std::ifstream input(utils::path_from_utf8(data_root_) / "preferences.json");
    auto saved = json::parse(input, nullptr, false);
    if (saved.is_object() && saved.value("enabled", false)) {
        std::string error;
        enable(error);
    }
    worker_ = std::thread(&NexusManager::supervise, this);
}

NexusManager::~NexusManager() { stop(); }
void NexusManager::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        wake_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
    management_server_.stop();
    if (management_thread_.joinable()) management_thread_.join();
    mesh_server_.stop();
    if (mesh_thread_.joinable()) mesh_thread_.join();
    std::lock_guard<std::mutex> session_lock(session_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    session_.reset();
}
NexusManager::json NexusManager::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = snapshot_;
    result["enabled"] = enabled_;
    result["controller_url"] = nexus_user_key::loopback_url(management_port_);
    result["remote_management"] = result.value("mesh_up", false);
    result["available"] = true;
    result["event_sequence"] = event_sequence_;
    if (!active_invite_.is_null()) result["active_invite"] = {{"device_name", active_invite_.value("device_name", "")}, {"expires_at", active_invite_.value("expires_at", uint64_t{0})}};
    const bool catalog = catalog_.is_object();
    result["catalog_version"] = catalog ? catalog_.value("version", uint64_t{0}) : uint64_t{0};
    result["app"] = catalog ? json(catalog_.at("app")) : json{{"name", "lemonade"}, {"version", LEMON_VERSION_STRING}};
    bool enforced = false;
    enforced = catalog && nexus_access::route_policy_available() && snapshot_.value("catalog_error", std::string()).empty();
    // Before the first tree request the SDK verifies the network's key history
    // page by page; on a long history a connect waits on it for several requests.
    {
        std::lock_guard<std::mutex> slot_lock(client_slot_->mutex);
        char* out = nullptr;
        if (client_slot_->client &&
            ln_tier1_progress(static_cast<ln_client_t*>(client_slot_->client), &out) == LN_OK && out) {
            const auto progress = json::parse(out, nullptr, false);
            if (progress.is_object() && progress.value("active", false)) {
                const auto total = progress.value("pages_total", uint64_t{0});
                const auto done = progress.value("pages_done", uint64_t{0});
                result["key_check"] = {{"epoch", progress.value("epoch", uint64_t{0})},
                                       {"target_epoch", progress.value("target", uint64_t{0})},
                                       {"page", total > 0 ? std::min(done + 1, total) : done + 1},
                                       {"pages", total}};
            }
        }
        ln_free(out);
    }
    result["route_policy"] = enforced ? "enforced" : "unavailable";
    return result;
}
void NexusManager::persist_enabled_locked() {
    const auto root = utils::path_from_utf8(data_root_);
    std::filesystem::create_directories(root);
    const auto temporary = root / "preferences.json.tmp";
    std::ofstream output(temporary);
    output << json{{"enabled", enabled_}}.dump();
    output.close();
    if (!output) throw std::runtime_error("Cannot save Nexus preferences");
    std::error_code ec;
    if (!utils::atomic_replace_file(temporary, root / "preferences.json", ec)) throw std::runtime_error("Cannot save Nexus preferences: " + ec.message());
}
bool NexusManager::enable(std::string& error) {
    // A new Session discovers the bootstrap server and loads the identity, which is
    // network I/O, so it is built under session_mutex_ alone (every writer of session_
    // holds it) and mutex_ is taken only to publish the result.
    std::lock_guard<std::mutex> session_lock(session_mutex_);
    std::unique_ptr<Session> failed;
    try {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) throw std::runtime_error("Server is shutting down");
        }
        std::unique_ptr<Session> fresh;
        if (!session_) fresh = std::make_unique<Session>(data_root_, ingress_port_, management_port_, selection_port_, catalog_, role_, client_slot_);
        const auto snapshot = (fresh ? fresh : session_)->status();
        std::lock_guard<std::mutex> lock(mutex_);
        if (fresh) session_ = std::move(fresh);
        enabled_ = true;
        persist_enabled_locked();
        update_snapshot_locked(snapshot);
        return true;
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_ = false;
        failed = std::move(session_);
        error = e.what();
        snapshot_["error"] = error;
        return false;
    }
}
void NexusManager::disable() {
    std::lock_guard<std::mutex> session_lock(session_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = false;
    session_.reset();
    active_invite_ = nullptr;
    update_snapshot_locked({{"status", nexus_access::name(SessionStatus::Disabled)}, {"locked", true}, {"mesh_up", false},
        {"members", json::array()}});
    persist_enabled_locked();
}
std::pair<int, NexusManager::json> NexusManager::control(Operation operation, const std::string& method, const std::string& node_id,
                                                       const json& body, const std::string& caller_key) {
    std::lock_guard<std::mutex> session_lock(session_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_ || !session_) return {409, {{"error", "Enable mesh first"}}};
    }
    std::pair<int, json> result;
    try { result = {200, session_->request(operation, method, node_id, body, caller_key)}; }
    catch (const NexusError& e) {
        result = {e.status, {{"error", e.what()}}};
        if (e.retryable) result.second["retryable"] = true;
    }
    catch (const json::exception&) { result = {400, {{"error", "Invalid request fields"}}}; }
    catch (const std::exception& e) { result = {502, {{"error", e.what()}}}; }
    json snapshot;
    try { snapshot = session_->status(); } catch (...) {}
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot.is_object()) update_snapshot_locked(snapshot);
    return result;
}

void NexusManager::emit_locked(EventType type, const json& data) {
    events_.push_back({{"sequence", ++event_sequence_}, {"type", nexus_access::name(type)}, {"data", data}});
    while (events_.size() > kMaxQueuedEvents) events_.pop_front();
    wake_.notify_all();
}

void NexusManager::update_snapshot_locked(const json& snapshot) {
    if (snapshot.value("locked", true) != snapshot_.value("locked", true)) {
        emit_locked(EventType::LockChanged, {{"locked", snapshot.value("locked", true)}});
    }
    if (snapshot.value("status", "") != snapshot_.value("status", "") ||
        snapshot.value("mesh_up", false) != snapshot_.value("mesh_up", false)) {
        emit_locked(EventType::MeshChanged, {{"status", snapshot.value("status", "")},
                                             {"mesh_up", snapshot.value("mesh_up", false)}});
    }
    const auto previous = snapshot_.value("members", json::array());
    const auto next = snapshot.value("members", json::array());
    if (snapshot.value("mesh_up", false) && snapshot_.value("mesh_up", false)) {
        for (const auto& member : next) {
            if (std::none_of(previous.begin(), previous.end(), [&](const json& old) {
                return old.value("node_id", "") == member.value("node_id", "");
            })) emit_locked(EventType::MemberJoined, member);
        }
        for (const auto& member : previous) {
            if (std::none_of(next.begin(), next.end(), [&](const json& now) {
                return now.value("node_id", "") == member.value("node_id", "");
            })) emit_locked(EventType::MemberLeft, member);
        }
    }
    snapshot_ = snapshot;
}

void NexusManager::supervise() {
    while (true) {
        {
            std::lock_guard<std::mutex> session_lock(session_mutex_);
            bool active = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                active = enabled_ && session_ && !stopping_;
            }
            if (active) {
                json snapshot;
                std::string error;
                try { session_->poll(); snapshot = session_->status(); }
                catch (const std::exception& e) { error = e.what(); }
                std::lock_guard<std::mutex> lock(mutex_);
                if (snapshot.is_object()) update_snapshot_locked(snapshot);
                else if (!error.empty()) snapshot_["error"] = error;
            }
        }
        std::unique_lock<std::mutex> lock(mutex_);
        if (wake_.wait_for(lock, kSupervisorPeriod, [this] { return stopping_; })) return;
    }
}

void NexusManager::handle(const httplib::Request& req, httplib::Response& res,
                           Operation operation, bool remote, const std::string& controller_url) {
    const auto reply = [&](int code, json body) {
        if (remote && body.is_object()) body.erase("controller_url");
        reply_json(res, code, body);
    };
    auto body = req.body.empty() ? json::object() : json::parse(req.body, nullptr, false);
    const std::size_t limit = operation == Operation::AccountAvatar ? kMaxAvatarImageBytes + kAvatarRequestEnvelopeBytes : kMaxRequestBodyBytes;
    if (!body.is_object() || req.body.size() > limit) {
        reply(400, {{"error", "Expected a bounded JSON object"}});
        return;
    }
    try {
        if (operation == Operation::Status || operation == Operation::JoinStatus) {
            auto current = status();
            if (!controller_url.empty()) current["controller_url"] = controller_url;
            reply(200, current);
            return;
        }
        if (operation == Operation::RoutePermissions) {
            if (!route_permissions_.is_object()) { reply_error(res, 503, "Route permissions are unavailable on this server"); return; }
            auto permissions = route_permissions_;
            const auto current = status();
            permissions["route_policy"] = current.at("route_policy");
            permissions["catalog_version"] = current.at("catalog_version");
            reply(200, permissions);
            return;
        }
        if (operation == Operation::Enable) {
            std::string error;
            if (!enable(error)) { reply(503, {{"error", error}}); return; }
            reply(200, status());
            return;
        }
        if (operation == Operation::Disable) { disable(); reply(200, status()); return; }
        if (operation == Operation::Events) {
            disable_caching(res);
            std::lock_guard<std::mutex> lock(mutex_);
            uint64_t after = 0;
            if (req.has_param("after")) after = std::stoull(req.get_param_value("after"));
            std::string stream;
            for (const auto& event : events_) {
                if (event.at("sequence").get<uint64_t>() <= after) continue;
                stream += "id: " + std::to_string(event.at("sequence").get<uint64_t>()) +
                    "\nevent: " + event.at("type").get<std::string>() + "\ndata: " + event.dump() + "\n\n";
            }
            if (req.has_param("once") && req.get_param_value("once") == "1") {
                res.set_content(stream.empty() ? ": heartbeat\n\n" : stream, kEventStreamContentType);
                return;
            }
            res.set_header("Connection", "keep-alive");
            res.set_header("X-Accel-Buffering", "no");
            res.set_chunked_content_provider(kEventStreamContentType, [this, after](size_t offset, httplib::DataSink& sink) mutable {
                std::string output;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    if (offset > 0) wake_.wait_for(lock, kSupervisorPeriod, [&] { return stopping_ || event_sequence_ > after; });
                    if (stopping_) { sink.done(); return false; }
                    for (const auto& event : events_) {
                        const auto sequence = event.at("sequence").get<uint64_t>();
                        if (sequence <= after) continue;
                        output += "id: " + std::to_string(sequence) + "\nevent: " + event.at("type").get<std::string>() + "\ndata: " + event.dump() + "\n\n";
                        after = sequence;
                    }
                }
                if (output.empty()) output = ": heartbeat\n\n";
                return sink.write(output.data(), output.size());
            });
            return;
        }
        const auto node_id = req.matches.size() > 1 ? req.matches[1].str() : std::string();
        auto result = control(operation, req.method, node_id, body, remote ? req.get_header_value(kPeerKeyHeader) : "");
        if (operation == Operation::PasskeyChallenge && result.first == 200) {
            result.second["registration"] = req.has_param("register") && req.get_param_value("register") == "1";
        }
        if ((operation == Operation::AccountDelete && result.first == 200 && result.second.value("status", "") == "deleted") ||
            (operation == Operation::Forget && result.first == 200)) disable();
        std::lock_guard<std::mutex> lock(mutex_);
        if (result.first == 200) {
            if (operation == Operation::Invites) active_invite_ = result.second;
            if (operation == Operation::InvitesCancel || operation == Operation::Lock) active_invite_ = nullptr;
            if (result.second.is_object() && result.second.contains("mesh_up")) update_snapshot_locked(result.second);
        }
        reply(result.first, result.second);
    } catch (const std::invalid_argument&) {
        reply(400, {{"error", "Invalid request fields"}});
    } catch (const std::exception& e) {
        reply(500, {{"error", e.what()}});
    }
}

void NexusManager::relay(const httplib::Request& req, httplib::Response& res) {
    uint16_t port = 0;
    {
        std::lock_guard<std::mutex> session_lock(session_mutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!enabled_ || !session_) { reply_error(res, 409, "Turn on the mesh first"); return; }
        }
        try { port = session_->relay_port(req.matches[1].str()); }
        catch (const NexusError& e) { reply_error(res, e.status, e.what()); return; }
    }
    auto path = req.matches[2].str();
    if (const auto query = req.target.find('?'); query != std::string::npos) path += req.target.substr(query);
    const auto node = req.matches[1].str();
    const auto reopen = [this, node]() -> uint16_t {
        std::lock_guard<std::mutex> session_lock(session_mutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!enabled_ || !session_) return 0;
        }
        try { return session_->reopen_relay_port(node); }
        catch (const std::exception& e) {
            LOG(WARNING, kLogTag) << "Cannot replace the mesh bridge to " << node << ": " << e.what() << std::endl;
            return 0;
        }
    };
    nexus_access::relay_http(port, node, path, req, res, req.get_header_value(nexus_access::kPeerAuthorizationHeader), req.body, reopen);
}

void NexusManager::register_routes(httplib::Server& server, NexusListener listener) {
    const auto dispatch = [this, listener](const httplib::Request& req, httplib::Response& res, Operation operation) {
        const auto access = nexus_access::route_access(listener, req, operation);
        if (access.first != 0) { reply_error(res, access.first, access.second); return; }
        // A loopback or local-network operator on the public port is told to keep using that
        // port: it is the one port an SSH tunnel, a VM or a container forwards, unlike the
        // ephemeral listener, and the only port a local-network client reaches.
        const bool local = listener != NexusListener::MeshIngress && private_network_address(req.remote_addr);
        const auto controller = listener == NexusListener::Public && local
            ? nexus_user_key::loopback_url(req.local_port) : std::string();
        handle(req, res, operation, !local, controller);
    };
    for (const auto& route : nexus_access::nexus_routes()) {
        auto pattern = std::string(nexus_prefix) + route.path;
        const auto parameter = pattern.find(kNodeIdParameter);
        if (parameter != std::string::npos) pattern.replace(parameter, kNodeIdParameter.size(), "([A-Za-z0-9_-]+)");
        const auto handler = [dispatch, operation = route.operation](const auto& req, auto& res) { dispatch(req, res, operation); };
        if (std::string_view(route.method) == "GET") server.Get(pattern, handler);
        else server.Post(pattern, handler);
    }
    const auto relayed = std::string(nexus_prefix) + R"(peers/([A-Za-z0-9_-]+)(/.*))";
    const auto relay_route = [this, listener](const httplib::Request& req, httplib::Response& res) {
        const auto access = nexus_access::route_access(listener, req, Operation::Relay);
        if (access.first != 0) { reply_error(res, access.first, access.second); return; }
        relay(req, res);
    };
    server.Get(relayed, relay_route);
    server.Post(relayed, relay_route);
}
}

#include "lemon/nexus_manager.h"
#include "lemon/error_types.h"
#include "lemon/streaming_proxy.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"
#include <lemon/utils/aixlog.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#ifdef LEMONADE_WITH_NEXUS
#include <LemonadeNexusSDK/lemonade_nexus.h>
#endif

namespace lemon {
namespace {
using json = nlohmann::json;
bool local_request(const httplib::Request& req) {
    return req.remote_addr == "127.0.0.1" || req.remote_addr == "::1" || req.remote_addr == "::ffff:127.0.0.1";
}
std::string api_key() { return utils::get_environment_variable_utf8("LEMONADE_API_KEY"); }
std::string admin_api_key() { return utils::get_environment_variable_utf8("LEMONADE_ADMIN_API_KEY"); }
class NexusError : public std::runtime_error {
public:
    NexusError(int status, const std::string& message) : std::runtime_error(message), status(status) {}
    int status;
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
json ordered(const json& permissions) {
    json result = json::array();
    for (const auto& name : nexus_access::kPermissions) {
        if (contains(permissions, name)) result.push_back(name);
    }
    for (const auto& name : permissions) {
        if (!contains(result, name.get<std::string>())) result.push_back(name);
    }
    return result;
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
void reply_error(httplib::Response& res, int status, const std::string& message) {
    res.set_header("Cache-Control", "no-store");
    res.status = status;
    res.set_content(json{{"error", message}}.dump(), "application/json");
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
}

namespace nexus_access {
const std::vector<std::string> kPermissions = {"read", "write", "add_child", "edit_node", "delete_node",
    "expand_subnet", "connect_private", "connect_shared", "relay_forward", "routing_coordinate",
    "stun_respond", "relay_register", "allocate_ip", "admin"};

bool same_key(const std::string& a, const std::string& b) {
    // The federation accepts either base64 spelling of a key, with or without the ed25519: prefix.
    const auto canonical = [](std::string key) {
        if (key.rfind("ed25519:", 0) == 0) key.erase(0, 8);
        std::replace(key.begin(), key.end(), '-', '+');
        std::replace(key.begin(), key.end(), '_', '/');
        while (!key.empty() && key.back() == '=') key.pop_back();
        return key;
    };
    const auto left = canonical(a);
    return !left.empty() && left == canonical(b);
}

bool grants(const json& permissions, const std::string& permission) {
    return contains(permissions, permission) || contains(permissions, "admin");
}

json account_summary(const json& members, const std::string& group_id,
                     const std::string& node_id, const std::string& identity_id) {
    const auto self = find_member(members, node_id);
    if (group_id.empty() || identity_id.empty() || self.is_null() || !self.value("permissions_known", false)) {
        throw NexusError(409, "Account membership could not be verified. Refresh the network and try again.");
    }
    return {{"id", group_id}, {"member_id", identity_id}, {"device_node_id", node_id},
        {"role", self.value("is_admin", false) ? "admin" : "user"}};
}

json account_avatar(const json& value, const std::string& group_id) {
    if (!value.is_object() || value.value("group_id", json()) != group_id ||
        !value.contains("revision") || !value["revision"].is_number_integer() ||
        (!value["revision"].is_number_unsigned() && value["revision"].get<int64_t>() < 0) ||
        value["revision"].get<uint64_t>() > 9007199254740991ULL ||
        !value.contains("image") || (!value["image"].is_null() && !value["image"].is_string()) ||
        !value.contains("can_write") || !value["can_write"].is_boolean() ||
        (value.contains("pending") && (!value["pending"].is_boolean() ||
            (value["pending"].get<bool>() && !value["image"].is_null())))) {
        throw NexusError(502, "The network returned invalid account image data");
    }
    if (value["image"].is_string()) {
        const auto& image = value["image"].get_ref<const std::string&>();
        if (image.size() > 128 * 1024 || image.rfind("data:image/png;base64,", 0) != 0) {
            throw NexusError(502, "The network returned an invalid profile image");
        }
    }
    return {{"state", "ready"}, {"revision", value["revision"]},
        {"image", value["image"]}, {"can_write", value["can_write"]}, {"pending", value.value("pending", false)}};
}

json tree_path(const std::string& id, const std::string& root_id,
               const std::function<json(const std::string&)>& fetch) {
    json path = json::array();
    std::set<std::string> visited;
    auto current = id;
    while (!current.empty() && visited.size() < 128) {
        if (current.size() > 256 || current.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos || !visited.insert(current).second) {
            throw NexusError(400, "Invalid group hierarchy");
        }
        auto node = fetch(current);
        if (!node.is_object() || text(node, "id") != current) throw NexusError(502, "The network returned an invalid node");
        if (!path.empty() && text(node, "type") != "customer" && text(node, "type") != "root") {
            throw NexusError(400, "Only groups can contain other nodes");
        }
        path.push_back(node);
        if (current == root_id) return path;
        current = text(node, "parent_id");
    }
    throw NexusError(403, "That node is outside your group or its hierarchy is too deep");
}

json tree_entry(const json& node, const std::string& self_key, bool ancestor_admin) {
    const auto type = text(node, "type");
    const bool group = type == "customer" || type == "root";
    const auto assignments = node.value("assignments", json::array());
    const auto permissions = permissions_for(assignments, self_key);
    const bool admin = ancestor_admin || contains(permissions, "admin");
    const bool access_known = grants(permissions, "edit_node");
    return {{"id", text(node, "id")}, {"parent_id", text(node, "parent_id")},
        {"name", text(node, "hostname")}, {"kind", group ? "group" : "node"},
        {"node_type", type}, {"tunnel_ip", text(node, "tunnel_ip")},
        {"is_inference", flag(node, "is_inference")}, {"management_pubkey", text(node, "mgmt_pubkey")},
        {"permissions", permissions}, {"can_add_group", group && (admin || grants(permissions, "add_child"))},
        {"can_rename", group && (admin || grants(permissions, "edit_node"))},
        {"can_manage", group && admin && access_known}, {"access_known", access_known},
        {"assignments", access_known ? assignments : json::array()}};
}

json merge_members(const json& group, const json& children, const json& peers,
                   const std::string& self_key, const std::string& self_node_id) {
    const auto group_acl = group.value("assignments", json::array());
    // The federation returns a node's full assignment list only to callers with edit_node on it.
    const bool group_acl_known = grants(permissions_for(group_acl, self_key), "edit_node");
    const auto creator = text(group, "mgmt_pubkey");
    json members = json::array();
    if (!children.is_array()) return members;
    for (const auto& child : children) {
        if (!child.is_object() || text(child, "type") != "endpoint") continue;
        const auto id = text(child, "id");
        if (id.empty()) continue;
        const auto key = text(child, "mgmt_pubkey");
        const bool self = id == self_node_id;
        const bool permissions_known = group_acl_known || self;
        const auto permissions = permissions_known ? permissions_for(group_acl, key) : json::array();
        const bool owner = same_key(key, creator);
        const auto device_acl = child.value("assignments", json::array());
        const auto own_device = permissions_for(device_acl, self_key);
        const bool access_known = grants(own_device, "edit_node");
        json access = json::array();
        if (access_known && device_acl.is_array()) {
            for (const auto& entry : device_acl) {
                if (!entry.is_object()) continue;
                access.push_back({{"management_pubkey", text(entry, "management_pubkey")},
                    {"permissions", string_list(entry.value("permissions", json::array()))}});
            }
        }
        const bool is_inference = flag(child, "is_inference");
        json peer = json::object();
        if (peers.is_array()) {
            for (const auto& candidate : peers) {
                if (candidate.is_object() && text(candidate, "node_id") == id) { peer = candidate; break; }
            }
        }
        members.push_back({{"node_id", id}, {"device_name", text(child, "hostname")},
            {"tunnel_ip", text(child, "tunnel_ip")}, {"management_pubkey", key},
            {"permissions", ordered(permissions)}, {"permissions_known", permissions_known},
            {"is_admin", permissions_known ? contains(permissions, "admin") : owner}, {"is_owner", owner},
            {"is_inference", is_inference}, {"tier", is_inference ? 3 : 4},
            {"access", access}, {"access_known", access_known},
            {"can_remove", !self && grants(own_device, "delete_node")},
            {"is_online", self || flag(peer, "is_online")},
            {"latency_ms", peer.contains("latency_ms") && peer["latency_ms"].is_number() ? peer["latency_ms"].get<double>() : 0.0}});
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
            {"is_admin", member.value("is_admin", false)}, {"tier", member.value("tier", 4)},
            {"see", grants(permissions, "read")}, {"connect", grants(permissions, "connect_private")},
            {"editable", can_manage && access_known && !contains(permissions, "admin")}});
    }
    return {{"node_id", device_id}, {"device_name", target.value("device_name", "")},
        {"access_known", access_known}, {"can_manage", can_manage}, {"members", rows}};
}

json role_permissions(const json& current, bool admin) {
    json result = json::array({"read"});
    for (const auto& name : string_list(current)) {
        if (name != "admin" && !contains(result, name.get<std::string>())) result.push_back(name);
    }
    if (admin) result.push_back("admin");
    return ordered(result);
}

json group_permissions(const json& requested, bool admin) {
    if (!requested.is_array()) throw NexusError(400, "Send permissions as a list of names");
    json result = json::array({"read"});
    for (const auto& name : requested) {
        if (!name.is_string()) throw NexusError(400, "Send permissions as a list of names");
        const auto value = name.get<std::string>();
        if (value == "admin") throw NexusError(400, "Change the member's role to grant or remove admin");
        if (std::find(kPermissions.begin(), kPermissions.end(), value) == kPermissions.end()) {
            throw NexusError(400, "Unknown permission: " + value);
        }
        if (!contains(result, value)) result.push_back(value);
    }
    if (admin) result.push_back("admin");
    return ordered(result);
}

json device_permissions(const json& current, bool see, bool connect) {
    json result = json::array();
    for (const auto& name : string_list(current)) {
        if (name != "read" && name != "connect_private") result.push_back(name);
    }
    if (see || connect) result.push_back("read");
    if (connect) result.push_back("connect_private");
    return ordered(result);
}

std::pair<int, std::string> assignment_error(const std::string& reason) {
    if (reason.find("only an admin") != std::string::npos) return {403, "Only a group admin can change member access"};
    if (reason.find("cannot change its own entry") != std::string::npos) return {403, "You cannot change your own admin access"};
    if (reason.find("has not joined") != std::string::npos) return {403, "That device has not joined your group"};
    if (reason.rfind("unknown permission", 0) == 0) return {400, "The network does not recognize that permission"};
    if (reason == "invalid principal") return {400, "That member has an invalid device key"};
    if (reason == "node not found") return {404, "That group or device no longer exists"};
    if (reason == "failed to persist node") return {502, "The network could not save the change. Try again."};
    if (reason == "server unreachable" || reason.empty()) return {502, "Cannot reach the network. Try again."};
    return {502, "The network refused the change: " + reason};
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

bool remote_operation(const std::string& operation) {
    static const std::vector<std::string> operations = {"status", "join/status", "devices", "members",
        "members/role", "members/permissions", "devices/access", "devices/remove", "invites", "invites/cancel", "join", "relay",
        "tree", "tree/children", "tree/groups", "tree/rename", "tree/permissions"};
    return std::find(operations.begin(), operations.end(), operation) != operations.end();
}

std::pair<int, std::string> route_access(NexusListener listener, bool loopback, const std::string& operation,
                                         const std::string& authorization, const std::string& api_key, const std::string& admin_key) {
    if (listener == NexusListener::MeshIngress) return {403, "Network management is not available through the mesh"};
    if (listener == NexusListener::Management) return loopback ? std::make_pair(0, std::string()) : std::make_pair(403, std::string("Local network management only"));
    if (loopback) return {operation == "status" || operation == "join/status" || operation == "relay" ? 0 : 307, ""};
    if (!remote_operation(operation)) return {403, "Use the Lemonade desktop app on this device for that action"};
    if (api_key.empty()) return {403, "Remote network management is off. Set LEMONADE_API_KEY on this server to turn it on."};
    if (!bearer_matches(authorization, api_key, admin_key)) return {401, "Invalid or missing API key"};
    return {0, ""};
}

const char* const kPeerAuthorizationHeader = "X-Lemonade-Peer-Authorization";

std::pair<int, std::string> relay_target(const json& members, const std::string& self_node_id, const std::string& node_id) {
    if (node_id == self_node_id) return {400, "Use this device directly instead of relaying to it"};
    const auto target = find_member(members, node_id);
    if (target.is_null()) return {404, "That device is not in your group"};
    if (!target.value("is_inference", false)) return {409, "That device is not an inference server"};
    if (!target.value("is_online", false)) return {409, "That device is offline"};
    auto address = target.value("tunnel_ip", "");
    if (const auto slash = address.find('/'); slash != std::string::npos) address.resize(slash);
    if (address.empty()) return {409, "That device has no mesh address"};
    return {0, address};
}

void relay_http(uint16_t port, const std::string& path, const httplib::Request& req, httplib::Response& res) {
    const auto url = "http://127.0.0.1:" + std::to_string(port) + path;
    std::map<std::string, std::string> headers;
    const auto peer_key = req.get_header_value(kPeerAuthorizationHeader);
    if (!peer_key.empty()) headers["Authorization"] = peer_key;
    if (req.has_header("X-Client-Session-Id")) headers["X-Client-Session-Id"] = req.get_header_value("X-Client-Session-Id");
    res.set_header("Cache-Control", "no-store");
    try {
        if (req.method == "GET") {
            const auto reply = utils::HttpClient::get(url, headers, 0, utils::HttpSecurityPolicy::TrustedLoopback);
            res.status = reply.status_code;
            res.set_content(reply.body, "application/json");
            return;
        }
        auto body = req.body;
        if (req.is_multipart_form_data()) {
            std::random_device random;
            const auto boundary = "lemonade-relay-" + std::to_string(random()) + std::to_string(random()) + std::to_string(random());
            headers["Content-Type"] = "multipart/form-data; boundary=" + boundary;
            body = multipart_body(req.form, boundary);
        } else {
            headers["Content-Type"] = req.has_header("Content-Type") ? req.get_header_value("Content-Type") : "application/json";
        }
        const auto request = json::parse(body, nullptr, false);
        if (request.is_object() && request.value("stream", false)) {
            // Same path as streamed backend responses: writes block on the client socket.
            res.set_chunked_content_provider("text/event-stream",
                [url, body, headers](size_t offset, httplib::DataSink& sink) {
                    if (offset > 0) return false;
                    try {
                        StreamingProxy::forward_byte_stream(url, body, sink, 300, nullptr, headers);
                    } catch (const std::exception& e) {
                        const auto error = "data: " + ErrorResponse::create(e.what(), "streaming_error").dump() + "\n\n";
                        sink.write(error.data(), error.size());
                        sink.done();
                    }
                    return false;
                });
            return;
        }
        const auto reply = utils::HttpClient::post(url, body, headers, 300, utils::HttpSecurityPolicy::TrustedLoopback);
        const auto type = reply.headers.find("content-type");
        res.status = reply.status_code;
        res.set_content(reply.body, type != reply.headers.end() ? type->second : "application/json");
    } catch (const std::exception&) {
        reply_error(res, 502, "Cannot reach that device's server through the mesh");
    }
}
}

#ifdef LEMONADE_WITH_NEXUS
namespace {
json sdk_call(const std::function<ln_error_t(char**)>& operation) {
    char* output = nullptr;
    const auto code = operation(&output);
    std::unique_ptr<char, decltype(&ln_free)> owned(output, ln_free);
    auto value = output ? json::parse(output, nullptr, false) : json::object();
    if (value.is_discarded()) throw NexusError(502, "The network returned invalid data");
    if (code != LN_OK) {
        const auto fallback = "Network request failed (SDK error " + std::to_string(code) + ")";
        throw NexusError(code == LN_ERR_AUTH ? 403 : code == LN_ERR_NOT_FOUND ? 404 : 502,
            value.is_object() ? value.value("error", fallback) : fallback);
    }
    return value;
}
std::string sdk_string(char* value) {
    std::unique_ptr<char, decltype(&ln_free)> owned(value, ln_free);
    return value ? value : "";
}
}
struct NexusManager::Session {
    std::unique_ptr<ln_client_t, decltype(&ln_destroy)> client{nullptr, ln_destroy};
    std::unique_ptr<ln_identity_t, decltype(&ln_identity_destroy)> identity{nullptr, ln_identity_destroy};
    std::filesystem::path root;
    int ingress_port;
    json account = json::object();
    bool locked{true};
    bool joined{false};
    bool reconnect{false};
    bool avatar_configured{false};
    std::chrono::steady_clock::time_point retry_at{};
    std::chrono::seconds retry_delay{5};
    std::map<std::string, std::string> egress_ips;
    std::string node_id;
    std::string group_id;
    std::string tunnel_ip;
    std::string host{"lemonade-nexus.io"};
    std::string rp_id{"lemonade-nexus.io"};
    std::string pubkey;
    std::string detail{"locked"};
    std::string error;
    json devices = json::array();
    std::chrono::system_clock::time_point last_poll{std::chrono::system_clock::now()};

    Session(const std::string& data_root, int port) : root(utils::path_from_utf8(data_root)), ingress_port(port) {
        std::filesystem::create_directories(root);
#ifndef _WIN32
        std::filesystem::permissions(root, std::filesystem::perms::owner_all);
#endif
        std::ifstream input(root / "account.json");
        if (input) {
            auto saved = json::parse(input, nullptr, false);
            if (!saved.is_object()) throw NexusError(500, "Device account is invalid; restore account.json");
            account = saved;
        }
        if (const auto value = utils::get_environment_variable_utf8("LEMONADE_NEXUS_SERVER"); !value.empty()) host = value;
        if (const auto value = utils::get_environment_variable_utf8("LEMONADE_NEXUS_RP_ID"); !value.empty()) rp_id = value;
        int public_port = 9100;
        const auto configured_port = utils::get_environment_variable_utf8("LEMONADE_NEXUS_PORT");
        if (!configured_port.empty()) public_port = std::stoi(configured_port);
        if (public_port < 1 || public_port > 65535) throw NexusError(400, "Invalid Nexus server port");
        client.reset(ln_create_tls(host.c_str(), static_cast<uint16_t>(public_port)));
        if (!client) throw NexusError(500, "Cannot create Nexus client");
        const auto path = utils::path_to_utf8(root / "identity.json");
        identity.reset(ln_identity_load(path.c_str()));
        if (!identity) {
            if (std::filesystem::exists(root / "identity.json")) throw NexusError(500, "Device identity is invalid; restore identity.json");
            identity.reset(ln_identity_generate());
            if (!identity || ln_identity_save(identity.get(), path.c_str()) != LN_OK) throw NexusError(500, "Cannot save device identity");
        }
        if (ln_set_identity(client.get(), identity.get()) != LN_OK) throw NexusError(500, "Cannot load device identity");
        pubkey = sdk_string(ln_identity_pubkey(identity.get()));
        const auto avatar_cache = utils::path_to_utf8(root / "avatar-cache");
        avatar_configured = ln_account_avatar_configure(client.get(), identity.get(), avatar_cache.c_str()) == LN_OK;
    }
    ~Session() { if (client) ln_mesh_disable(client.get()); }
    std::string device_name() const { return account.value("device_name", "My Lemonade device"); }
    void persist() {
        const auto temporary = root / "account.json.tmp";
        std::ofstream output(temporary);
        output << account.dump(2);
        output.close();
        if (!output) throw NexusError(500, "Cannot save device account");
        std::error_code ec;
        if (!utils::atomic_replace_file(temporary, root / "account.json", ec)) throw NexusError(500, "Cannot replace device account");
    }
    void lock() {
        locked = true;
        joined = false;
        reconnect = false;
        egress_ips.clear();
        ln_mesh_disable(client.get());
        ln_set_session_token(client.get(), "");
        ln_set_link_token(client.get(), "");
        devices = json::array();
        detail = "locked";
        error.clear();
    }
    json authenticate_identity() {
        auto result = sdk_call([&](auto out) { return ln_auth_ed25519(client.get(), out); });
        if (account.contains("user_id") && account.at("user_id") != result.at("user_id")) {
            throw NexusError(403, "Account does not belong to this device identity");
        }
        account["user_id"] = result.at("user_id");
        return result;
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
            throw NexusError(502, reason.empty() ? "Network request failed" : reason);
        }
        return value;
    }

    json read_avatar() {
        if (!avatar_configured) return {{"state", "unavailable"}};
        char* output = nullptr;
        const auto code = ln_account_avatar_get(client.get(), &output);
        std::unique_ptr<char, decltype(&ln_free)> owned(output, ln_free);
        if (code == LN_ERR_NOT_FOUND || code == LN_ERR_UNSUPPORTED) return {{"state", "unsupported"}};
        if (code != LN_OK || !output) return {{"state", "unavailable"}};
        try { return nexus_access::account_avatar(json::parse(output), group_id); }
        catch (const std::exception&) { return {{"state", "unavailable"}}; }
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
        auto children = sdk_call([&](auto out) { return ln_tree_get_children(client.get(), group_id.c_str(), out); });
        if (!children.is_array()) throw NexusError(502, "The network returned invalid data");
        return children;
    }
    json scoped_tree_path(const std::string& id) {
        return nexus_access::tree_path(id, group_id, [&](const std::string& current) {
            return private_call("GET", "/api/tree/node/" + current);
        });
    }
    json describe_tree_node(const json& path) {
        bool ancestor_admin = false;
        for (size_t i = 1; i < path.size(); ++i) {
            ancestor_admin = ancestor_admin || contains(permissions_for(path[i].value("assignments", json::array()), pubkey), "admin");
        }
        return nexus_access::tree_entry(path.front(), pubkey, ancestor_admin);
    }
    json tree_children(const std::string& id) {
        const auto path = scoped_tree_path(id);
        const auto parent = describe_tree_node(path);
        if (parent.at("kind") != "group") throw NexusError(400, "Only groups can contain other nodes");
        const auto children = private_call("GET", "/api/tree/children/" + id);
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
        const auto own = sdk_call([&](auto out) { return ln_tree_get_node(client.get(), node_id.c_str(), out); });
        group_id = own.at("parent_id").get<std::string>();
        const auto group = sdk_call([&](auto out) { return ln_tree_get_node(client.get(), group_id.c_str(), out); });
        const auto children = group_children();
        const auto peers = sdk_call([&](auto out) { return ln_mesh_peers(client.get(), out); });
        auto next = nexus_access::merge_members(group, children, peers, pubkey, node_id);
        for (auto it = egress_ips.begin(); it != egress_ips.end();) {
            const auto present = std::find_if(next.begin(), next.end(), [&](const json& device) {
                auto ip = device.value("tunnel_ip", "");
                if (const auto slash = ip.find('/'); slash != std::string::npos) ip.resize(slash);
                return device.value("node_id", "") == it->first && ip == it->second;
            });
            if (present == next.end()) {
                ln_mesh_close_egress(client.get(), it->second.c_str(), 13305);
                it = egress_ips.erase(it);
            } else ++it;
        }
        devices = std::move(next);
    }
    uint16_t relay_port(const std::string& id) {
        if (locked) throw NexusError(423, "Unlock your network first");
        if (!joined) throw NexusError(409, "Join the network first");
        const auto target = nexus_access::relay_target(devices, node_id, id);
        if (target.first != 0) throw NexusError(target.first, target.second);
        uint16_t port = 0;
        if (ln_mesh_open_egress(client.get(), target.second.c_str(), 13305, &port) != LN_OK || !port) {
            throw NexusError(502, "Cannot open a mesh connection to that device");
        }
        egress_ips[id] = target.second;
        return port;
    }
    json status() {
        auto mesh = sdk_call([&](auto out) { return ln_mesh_status(client.get(), out); });
        return {{"status", detail}, {"locked", locked}, {"mesh_up", !locked && ln_mesh_is_active(client.get()) != 0},
            {"node_id", node_id}, {"group_node_id", group_id}, {"tunnel_ip", tunnel_ip},
            {"peer_count", mesh.value("peer_count", 0)}, {"device_name", device_name()}, {"server", host},
            {"registered", account.contains("credential_id")}, {"members", devices},
            {"exposed", json::array({{{"vport", 13305}, {"local_port", ingress_port}}})}, {"error", error}};
    }
    void join(const std::string& code) {
        if (locked) throw NexusError(423, "Unlock with your passkey before joining");
        if (ingress_port <= 0) throw NexusError(503, "The mesh service listener is not running");
        detail = "joining";
        ln_set_link_token(client.get(), code.c_str());
        if (ln_set_inference_node(client.get(), 1) != LN_OK) throw NexusError(500, "Cannot join as an inference node");
        json joined_result;
        try { joined_result = sdk_call([&](auto out) { return ln_join_network(client.get(), device_name().c_str(), "", out); }); }
        catch (const std::exception& e) {
            ln_set_link_token(client.get(), "");
            detail = "degraded";
            error = e.what();
            throw;
        }
        ln_set_link_token(client.get(), "");
        node_id = joined_result.at("node_id").get<std::string>();
        tunnel_ip = joined_result.at("tunnel_ip").get<std::string>();
        if (ln_mesh_enable(client.get()) != LN_OK || ln_mesh_expose_service(client.get(), 13305,
                ("tcp:127.0.0.1:" + std::to_string(ingress_port)).c_str()) != LN_OK) {
            lock();
            throw NexusError(502, "Could not start the mesh transport");
        }
        joined = true;
        reconnect = true;
        retry_delay = std::chrono::seconds(5);
        detail = "running";
        error.clear();
        try {
            const auto updates = json{{"hostname", device_name()}}.dump();
            sdk_call([&](auto out) { return ln_update_node(client.get(), node_id.c_str(), updates.c_str(), out); });
            refresh_devices();
        } catch (const std::exception& e) { error = e.what(); }
    }
    void poll() {
        const auto now = std::chrono::system_clock::now();
        const auto elapsed = now - last_poll;
        last_poll = now;
        if (!locked && elapsed > std::chrono::minutes(5)) { lock(); return; }
        if (locked || !reconnect || std::chrono::steady_clock::now() < retry_at) return;
        try {
            if (!joined || !ln_mesh_is_active(client.get())) {
                ln_mesh_disable(client.get());
                egress_ips.clear();
                joined = false;
                join("");
            }
            else { refresh_devices(); error.clear(); }
        } catch (const std::exception& e) {
            error = e.what(); detail = "degraded";
            retry_at = std::chrono::steady_clock::now() + retry_delay;
            retry_delay = std::min(retry_delay * 2, std::chrono::seconds(60));
        }
    }
    json request(const std::string& method, const std::string& path, const json& body) {
        if (path == "/status") return status();
        if (path == "/device") {
            const auto name = body.at("device_name").get<std::string>();
            if (name.empty() || name.size() > 128) throw NexusError(400, "Enter a device name (up to 128 characters)");
            if (joined && !locked) {
                const auto updates = json{{"hostname", name}}.dump();
                sdk_call([&](auto out) { return ln_update_node(client.get(), node_id.c_str(), updates.c_str(), out); });
            }
            account["device_name"] = name;
            persist();
            return status();
        }
        if (path.rfind("/passkey/challenge", 0) == 0) {
            const auto authenticated = authenticate_identity();
            const auto user_id = authenticated.at("user_id").get<std::string>();
            const auto challenge = sdk_call([&](auto out) { return ln_auth_passkey_challenge(client.get(), user_id.c_str(), out); });
            return {{"challenge", challenge.at("challenge")}, {"user_id", user_id}, {"rp_id", rp_id},
                {"credential_id", account.value("credential_id", "")}, {"registration", path.find("register=1") != std::string::npos}};
        }
        if (path == "/passkey/register") {
            if (account.contains("credential_id")) throw NexusError(409, "This device already has a passkey");
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
        if (path == "/unlock") {
            const auto& assertion = body.at("passkey_assertion");
            if (!account.contains("credential_id") || assertion.at("credential_id") != account.at("credential_id")) throw NexusError(403, "Use this device's registered passkey");
            auto encoded = assertion.at("authenticator_data").get<std::string>();
            std::replace(encoded.begin(), encoded.end(), '-', '+');
            std::replace(encoded.begin(), encoded.end(), '_', '/');
            while (encoded.size() % 4) encoded += '=';
            const auto bytes = utils::JsonUtils::base64_decode(encoded);
            if (bytes.size() < 37 || (static_cast<unsigned char>(bytes[32]) & 0x05) != 0x05) throw NexusError(403, "Passkey user verification is required");
            const auto payload = json{{"assertion", assertion}}.dump();
            const auto authenticated = sdk_call([&](auto out) { return ln_auth_passkey(client.get(), payload.c_str(), out); });
            if (authenticated.at("user_id") != account.at("user_id")) {
                ln_set_session_token(client.get(), "");
                throw NexusError(403, "Passkey does not match this device identity");
            }
            authenticate_identity();
            locked = false;
            last_poll = std::chrono::system_clock::now();
            detail = joined ? "running" : "ready";
            if (!body.value("defer_join", false) && !joined) join("");
            return status();
        }
        if (path == "/lock") { lock(); return status(); }
        if (path == "/link-token/cancel") return {{"cancelled", true}, {"revoked", false}};
        if (locked) throw NexusError(423, "Unlock your network first");
        if (path == "/join") {
            const auto code = body.at("code").get<std::string>();
            const bool full_token = code.size() == 68 && code.rfind("lnk_", 0) == 0 && code.substr(4).find_first_not_of("0123456789abcdef") == std::string::npos;
            if (!full_token && (code.size() != 8 || code.find_first_not_of("0123456789") != std::string::npos)) throw NexusError(400, "Enter an 8-digit code or a full invite token");
            if (joined) throw NexusError(409, "Lock this device before joining another group");
            join(code);
            return status();
        }
        if (!joined) throw NexusError(409, "Join the network first");
        if (path == "/account") {
            refresh_devices();
            auto result = nexus_access::account_summary(devices, group_id, node_id, account.value("user_id", ""));
            result["devices"] = devices;
            result["avatar"] = read_avatar();
            return result;
        }
        if (path == "/account/avatar" && method == "POST") {
            if (body.value("account_id", "") != group_id || body.value("member_id", "") != account.value("user_id", "") ||
                body.value("device_node_id", "") != node_id) throw NexusError(409, "The account changed. Sign in again.");
            const auto current = read_avatar();
            if (current.at("state") == "unsupported") throw NexusError(501, "This mesh needs a Nexus server update to sync profile images.");
            if (current.at("state") != "ready") throw NexusError(502, "Cannot reach the account image service. Try again.");
            if (!current.at("can_write").get<bool>()) throw NexusError(403, "You do not have permission to change this account image.");
            if (body.at("revision") != current.at("revision")) throw NexusError(409, "The profile image changed. Refresh and try again.");
            const auto revision = current.at("revision").get<uint64_t>();
            if (revision >= 9007199254740991ULL) throw NexusError(409, "The account image revision limit was reached.");
            const auto& image = body.at("image");
            if (!image.is_null() && !image.is_string()) throw NexusError(400, "Expected a profile image or null");
            const auto data = image.is_null() ? std::string() : image.get<std::string>();
            if (data.size() > 128 * 1024 || (!data.empty() && data.rfind("data:image/png;base64,", 0) != 0)) throw NexusError(400, "Choose a PNG profile image under 128 KiB.");
            const auto record = sdk_call([&](auto out) { return ln_account_avatar_sign(identity.get(), group_id.c_str(), data.c_str(), revision + 1, out); });
            char* output = nullptr;
            const auto encoded = record.dump();
            const auto code = ln_account_avatar_put(client.get(), encoded.c_str(), data.c_str(), &output);
            std::unique_ptr<char, decltype(&ln_free)> owned(output, ln_free);
            const auto result = output ? json::parse(output, nullptr, false) : json();
            if (code != LN_OK) {
                const auto message = result.is_object() ? result.value("error", "Profile image could not be saved") : "Profile image could not be saved";
                throw NexusError(code == LN_ERR_AUTH ? 403 : code == LN_ERR_REJECTED ? 409 : code == LN_ERR_NOT_FOUND ? 501 : 502, message);
            }
            return nexus_access::account_avatar(result, group_id);
        }
        if (path == "/tree") return tree_children(group_id);
        if (path.rfind("/tree/", 0) == 0) {
            const auto separator = path.find('/', 6);
            if (separator == std::string::npos) throw NexusError(404, "Unknown tree operation");
            const auto id = path.substr(6, separator - 6);
            const auto action = path.substr(separator + 1);
            if (method == "GET" && action == "children") return tree_children(id);
            const auto chain = scoped_tree_path(id);
            const auto target = describe_tree_node(chain);
            if (target.at("kind") != "group") throw NexusError(400, "Select a group for this action");
            if (method == "POST" && (action == "groups" || action == "rename")) {
                if (!target.at(action == "groups" ? "can_add_group" : "can_rename").get<bool>()) throw NexusError(403, "You do not have permission to change this group");
                const auto name = body.at("name").get<std::string>();
                if (name.empty() || name.size() > 128 || name.find_first_not_of(" \t\r\n") == std::string::npos) throw NexusError(400, "Enter a group name (up to 128 UTF-8 bytes)");
                const auto result = action == "groups"
                    ? private_call("POST", "/api/tree/node", json{{"parent_id", id}, {"type", "customer"}, {"hostname", name}}.dump())
                    : private_call("POST", "/api/tree/node/update/" + id, json{{"hostname", name}}.dump());
                if (!result.value("success", false)) throw NexusError(502, "The network could not save the group");
                return result;
            }
            if (method == "POST" && action == "permissions") {
                if (!target.at("can_manage").get<bool>()) throw NexusError(403, "Only an admin with access to this group's assignments can change permissions");
                const auto member = scoped_tree_path(body.at("member_node_id").get<std::string>()).front();
                if (text(member, "type") != "endpoint") throw NexusError(400, "Choose a device node");
                const auto key = text(member, "mgmt_pubkey");
                if (key.empty() || nexus_access::same_key(key, pubkey)) throw NexusError(403, "You cannot change your own access");
                const bool admin = body.at("admin").get<bool>();
                if (!admin && nexus_access::same_key(key, text(chain.front(), "mgmt_pubkey"))) throw NexusError(403, "The group creator always administers the group");
                const auto permissions = nexus_access::group_permissions(body.at("permissions"), admin);
                set_assignment(id, key, permissions);
                return tree_children(id);
            }
            throw NexusError(404, "Unknown tree operation");
        }
        if (path == "/devices") { refresh_devices(); return devices; }
        if (path == "/members") {
            refresh_devices();
            return {{"group_node_id", group_id}, {"can_manage", group_admin()}, {"members", devices}};
        }
        if (path == "/link-token") {
            const auto ttl = body.value("ttl_sec", 600);
            if (ttl < 60 || ttl > 600) throw NexusError(400, "Invite lifetime must be 60 to 600 seconds");
            const auto name = body.at("device_name").get<std::string>();
            if (name.empty() || name.size() > 128) throw NexusError(400, "Enter the name of the device you're inviting");
            auto result = sdk_call([&](auto out) { return ln_link_token_create(client.get(), ttl, out); });
            result["device_name"] = name;
            return result;
        }
        refresh_devices();
        if (path == "/egress") {
            const auto id = body.at("node_id").get<std::string>();
            const auto target = find_member(devices, id);
            if (target.is_null() || id == node_id) throw NexusError(404, "Choose another member of your group");
            auto ip = target.at("tunnel_ip").get<std::string>();
            if (auto slash = ip.find('/'); slash != std::string::npos) ip.resize(slash);
            uint16_t port = 0;
            if (ln_mesh_open_egress(client.get(), ip.c_str(), 13305, &port) != LN_OK || !port) throw NexusError(502, "Remote server connection failed");
            egress_ips[id] = ip;
            return {{"node_id", id}, {"loopback_port", port}, {"endpoint", "http://127.0.0.1:" + std::to_string(port)}};
        }
        const auto first = path.find('/', 1);
        const auto second = first == std::string::npos ? std::string::npos : path.find('/', first + 1);
        if (second == std::string::npos) throw NexusError(404, "Unknown network operation");
        const auto collection = path.substr(0, first);
        const auto id = path.substr(first + 1, second - first - 1);
        const auto action = path.substr(second + 1);
        const auto target = find_member(devices, id);
        if (target.is_null()) throw NexusError(404, "That device is not in your group");
        if (collection == "/devices" && action == "remove") {
            if (id == node_id) throw NexusError(404, "Choose another member of your group");
            if (!target.at("can_remove").get<bool>()) throw NexusError(403, "This device does not have permission to remove that device");
            const auto removed_key = target.at("management_pubkey").get<std::string>();
            private_call("POST", "/api/tree/node/delete/" + id, "{}");
            if (group_admin()) set_assignment(group_id, removed_key, json::array());
            refresh_devices();
            return {{"removed", true}};
        }
        if (collection == "/devices" && action == "access") {
            if (method == "POST") {
                if (!group_admin()) throw NexusError(403, "Only a group admin can change device access");
                if (!target.at("access_known").get<bool>()) {
                    throw NexusError(409, "This device's access list is not visible to you. Ask the admin who invited it.");
                }
                const auto member_id = body.at("member_node_id").get<std::string>();
                const bool connect = body.at("connect").get<bool>();
                const bool see = body.at("see").get<bool>() || connect;
                if (member_id == node_id) throw NexusError(403, "You cannot change your own access");
                const auto member = find_member(devices, member_id);
                if (member.is_null() || member_id == id) throw NexusError(404, "Choose another member of your group");
                const auto key = member.at("management_pubkey").get<std::string>();
                const auto current = permissions_for(target.at("access"), key);
                if (contains(current, "admin")) throw NexusError(409, "That member administers this device; change its role instead");
                set_assignment(id, key, nexus_access::device_permissions(current, see, connect));
                refresh_devices();
            }
            return nexus_access::device_access(devices, id, node_id);
        }
        if (collection == "/members" && (action == "role" || action == "permissions")) {
            if (!group_admin()) throw NexusError(403, "Only a group admin can change member access");
            if (id == node_id) throw NexusError(403, action == "role" ? "You cannot change your own role. Ask another admin." : "You cannot change your own access. Ask another admin.");
            const auto key = target.at("management_pubkey").get<std::string>();
            const bool admin = target.at("is_admin").get<bool>();
            json permissions;
            if (action == "role") {
                const auto role = body.at("role").get<std::string>();
                if (role != "admin" && role != "member") throw NexusError(400, "Choose the admin or member role");
                if (role == "member" && target.at("is_owner").get<bool>()) throw NexusError(403, "The group creator always administers the group");
                permissions = nexus_access::role_permissions(target.at("permissions"), role == "admin");
            } else {
                permissions = nexus_access::group_permissions(body.at("permissions"), admin);
            }
            set_assignment(group_id, key, permissions);
            refresh_devices();
            return find_member(devices, id);
        }
        throw NexusError(404, "Unknown network operation");
    }
};
#else
struct NexusManager::Session {};
#endif

NexusManager::NexusManager(const std::string& config_dir,
                           const std::function<void(httplib::Server&)>& install_policy,
                           const std::function<void(httplib::Server&)>& install_api)
    : data_root_(utils::path_to_utf8(utils::path_from_utf8(config_dir) / "nexus")) {
    // Built once and never rebound: a main-port rebind leaves the mesh listener alone.
    register_routes(mesh_server_, NexusListener::MeshIngress);
    install_api(mesh_server_);
    mesh_server_.set_read_timeout(30, 0);
    mesh_server_.set_write_timeout(300, 0);
    ingress_port_ = std::max(mesh_server_.bind_to_any_port("127.0.0.1"), 0);
    if (ingress_port_ > 0) {
        mesh_thread_ = std::thread([this] { mesh_server_.listen_after_bind(); });
        mesh_server_.wait_until_ready();
    } else {
        LOG(WARNING, "Nexus") << "Cannot bind the mesh listener; mesh peers cannot reach this server" << std::endl;
    }
#ifdef LEMONADE_WITH_NEXUS
    std::ifstream input(utils::path_from_utf8(data_root_) / "preferences.json");
    auto saved = json::parse(input, nullptr, false);
    if (saved.is_object() && saved.value("enabled", false)) {
        std::string error;
        enable(error);
    }
    worker_ = std::thread(&NexusManager::supervise, this);
#endif
    register_routes(management_server_, NexusListener::Management);
    install_policy(management_server_);
    management_port_ = management_server_.bind_to_any_port("127.0.0.1");
    if (management_port_ < 0) { stop(); throw std::runtime_error("Cannot bind local network management listener"); }
    management_thread_ = std::thread([this] { management_server_.listen_after_bind(); });
    management_server_.wait_until_ready();
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
    result["controller_url"] = "http://127.0.0.1:" + std::to_string(management_port_);
    result["remote_management"] = !api_key().empty();
#ifdef LEMONADE_WITH_NEXUS
    result["available"] = true;
#else
    result["available"] = false;
#endif
    result["event_sequence"] = event_sequence_;
    if (!active_invite_.is_null()) result["active_invite"] = {{"device_name", active_invite_.value("device_name", "")}, {"expires_at", active_invite_.value("expires_at", uint64_t{0})}};
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
    std::lock_guard<std::mutex> session_lock(session_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
#ifndef LEMONADE_WITH_NEXUS
    error = "This build does not include the Nexus SDK";
    return false;
#else
    try {
        if (stopping_) throw std::runtime_error("Server is shutting down");
        if (!session_) session_ = std::make_unique<Session>(data_root_, ingress_port_);
        enabled_ = true;
        persist_enabled_locked();
        update_snapshot_locked(session_->status());
        return true;
    } catch (const std::exception& e) {
        enabled_ = false;
        session_.reset();
        error = e.what();
        snapshot_["error"] = error;
        return false;
    }
#endif
}
void NexusManager::disable() {
    std::lock_guard<std::mutex> session_lock(session_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = false;
    session_.reset();
    active_invite_ = nullptr;
    update_snapshot_locked({{"status", "disabled"}, {"locked", true}, {"mesh_up", false}, {"members", json::array()}});
    persist_enabled_locked();
}
std::pair<int, NexusManager::json> NexusManager::control(const std::string& method, const std::string& path, const json& body) {
#ifdef LEMONADE_WITH_NEXUS
    // session_mutex_ serializes SDK work; mutex_ guards only the published state, so status,
    // events and shutdown do not wait for a slow federation request.
    std::lock_guard<std::mutex> session_lock(session_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_ || !session_) return {409, {{"error", "Enable mesh first"}}};
    }
    std::pair<int, json> result;
    try { result = {200, session_->request(method, path, body)}; }
    catch (const NexusError& e) { result = {e.status, {{"error", e.what()}}}; }
    catch (const json::exception&) { result = {400, {{"error", "Invalid request fields"}}}; }
    catch (const std::exception& e) { result = {502, {{"error", e.what()}}}; }
    json snapshot;
    try { snapshot = session_->status(); } catch (...) {}
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot.is_object()) update_snapshot_locked(snapshot);
    return result;
#else
    return {503, {{"error", "This build does not include the Nexus SDK"}}};
#endif
}

void NexusManager::emit_locked(const std::string& type, const json& data) {
    events_.push_back({{"sequence", ++event_sequence_}, {"type", type}, {"data", data}});
    while (events_.size() > 128) events_.pop_front();
    wake_.notify_all();
}

void NexusManager::update_snapshot_locked(const json& snapshot) {
    if (snapshot.value("locked", true) != snapshot_.value("locked", true)) {
        emit_locked("nexus.lock_changed", {{"locked", snapshot.value("locked", true)}});
    }
    if (snapshot.value("status", "") != snapshot_.value("status", "") ||
        snapshot.value("mesh_up", false) != snapshot_.value("mesh_up", false)) {
        emit_locked("nexus.mesh_changed", {{"status", snapshot.value("status", "")},
                                          {"mesh_up", snapshot.value("mesh_up", false)}});
    }
    const auto previous = snapshot_.value("members", json::array());
    const auto next = snapshot.value("members", json::array());
    if (snapshot.value("mesh_up", false) && snapshot_.value("mesh_up", false)) {
        for (const auto& member : next) {
            if (std::none_of(previous.begin(), previous.end(), [&](const json& old) {
                return old.value("node_id", "") == member.value("node_id", "");
            })) emit_locked("nexus.member_joined", member);
        }
        for (const auto& member : previous) {
            if (std::none_of(next.begin(), next.end(), [&](const json& now) {
                return now.value("node_id", "") == member.value("node_id", "");
            })) emit_locked("nexus.member_left", member);
        }
    }
    snapshot_ = snapshot;
}

void NexusManager::supervise() {
    while (true) {
#ifdef LEMONADE_WITH_NEXUS
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
#endif
        std::unique_lock<std::mutex> lock(mutex_);
        if (wake_.wait_for(lock, std::chrono::seconds(5), [this] { return stopping_; })) return;
    }
}

void NexusManager::handle(const httplib::Request& req, httplib::Response& res,
                           const std::string& operation, bool remote) {
    res.set_header("Cache-Control", "no-store");
    const auto reply = [&](int code, const json& body) {
        res.status = code;
        res.set_content(body.dump(), "application/json");
    };
    auto body = req.body.empty() ? json::object() : json::parse(req.body, nullptr, false);
    const std::size_t limit = operation == "account/avatar" ? 128 * 1024 + 2048 : 65536;
    if (!body.is_object() || req.body.size() > limit) {
        reply(400, {{"error", "Expected a bounded JSON object"}});
        return;
    }
    try {
        if (operation == "status" || operation == "join/status") {
            auto current = status();
            if (remote) current.erase("controller_url");
            reply(200, current);
            return;
        }
        if (operation == "enable") {
            std::string error;
            if (!enable(error)) { reply(503, {{"error", error}}); return; }
            reply(200, status());
            return;
        }
        if (operation == "disable") { disable(); reply(200, status()); return; }
        if (operation == "events") {
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
                res.set_content(stream.empty() ? ": heartbeat\n\n" : stream, "text/event-stream");
                return;
            }
            res.set_header("Connection", "keep-alive");
            res.set_header("X-Accel-Buffering", "no");
            res.set_chunked_content_provider("text/event-stream", [this, after](size_t offset, httplib::DataSink& sink) mutable {
                std::string output;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    if (offset > 0) wake_.wait_for(lock, std::chrono::seconds(5), [&] { return stopping_ || event_sequence_ > after; });
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
        std::string path;
        if (operation == "passkey/challenge") {
            path = "/passkey/challenge";
            if (req.has_param("register") && req.get_param_value("register") == "1") path += "?register=1";
        } else if (operation == "passkey/register") path = "/passkey/register";
        else if (operation == "passkey/unlock") path = "/unlock";
        else if (operation == "invites") path = "/link-token";
        else if (operation == "invites/cancel") path = "/link-token/cancel";
        else if (operation == "devices/remove") path = "/devices/" + req.matches[1].str() + "/remove";
        else if (operation == "devices/access") path = "/devices/" + req.matches[1].str() + "/access";
        else if (operation == "members/role") path = "/members/" + req.matches[1].str() + "/role";
        else if (operation == "members/permissions") path = "/members/" + req.matches[1].str() + "/permissions";
        else if (operation.rfind("tree/", 0) == 0) path = "/tree/" + req.matches[1].str() + "/" + operation.substr(5);
        else path = "/" + operation;
        auto result = control(req.method, path, body);
        std::lock_guard<std::mutex> lock(mutex_);
        if (result.first == 200) {
            if (operation == "invites") active_invite_ = result.second;
            if (operation == "invites/cancel" || operation == "lock") active_invite_ = nullptr;
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
#ifdef LEMONADE_WITH_NEXUS
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
    nexus_access::relay_http(port, path, req, res);
#else
    reply_error(res, 503, "This build does not include the Nexus SDK");
#endif
}

void NexusManager::register_routes(httplib::Server& server, NexusListener listener) {
    const auto dispatch = [this, listener](const httplib::Request& req, httplib::Response& res, const std::string& operation) {
        const bool loopback = local_request(req);
        const auto access = nexus_access::route_access(listener, loopback, operation,
            req.get_header_value("Authorization"), api_key(), admin_api_key());
        if (access.first == 307) {
            res.set_redirect("http://127.0.0.1:" + std::to_string(management_port_) + req.target, 307);
            return;
        }
        if (access.first != 0) {
            res.set_header("Cache-Control", "no-store");
            res.status = access.first;
            res.set_content(json{{"error", access.second}}.dump(), "application/json");
            return;
        }
        handle(req, res, operation, listener == NexusListener::Public && !loopback);
    };
    for (const auto* prefix : {"/api/v0/nexus/", "/api/v1/nexus/", "/v0/nexus/", "/v1/nexus/", "/api/nexus/"}) {
        for (const auto* operation : {"status", "passkey/challenge", "devices", "members", "join/status", "events", "tree", "account"}) {
            server.Get(std::string(prefix) + operation, [dispatch, operation](const auto& req, auto& res) { dispatch(req, res, operation); });
        }
        for (const auto* operation : {"enable", "disable", "passkey/register", "passkey/unlock", "invites", "invites/cancel", "join", "egress", "lock", "device", "account/avatar"}) {
            server.Post(std::string(prefix) + operation, [dispatch, operation](const auto& req, auto& res) { dispatch(req, res, operation); });
        }
        server.Post(std::string(prefix) + R"(devices/([A-Za-z0-9_-]+)/remove)",
            [dispatch](const auto& req, auto& res) { dispatch(req, res, "devices/remove"); });
        server.Get(std::string(prefix) + R"(devices/([A-Za-z0-9_-]+)/access)",
            [dispatch](const auto& req, auto& res) { dispatch(req, res, "devices/access"); });
        server.Post(std::string(prefix) + R"(devices/([A-Za-z0-9_-]+)/access)",
            [dispatch](const auto& req, auto& res) { dispatch(req, res, "devices/access"); });
        server.Post(std::string(prefix) + R"(members/([A-Za-z0-9_-]+)/role)",
            [dispatch](const auto& req, auto& res) { dispatch(req, res, "members/role"); });
        server.Post(std::string(prefix) + R"(members/([A-Za-z0-9_-]+)/permissions)",
            [dispatch](const auto& req, auto& res) { dispatch(req, res, "members/permissions"); });
        server.Get(std::string(prefix) + R"(tree/([A-Za-z0-9_-]+)/children)",
            [dispatch](const auto& req, auto& res) { dispatch(req, res, "tree/children"); });
        for (const auto* action : {"groups", "rename", "permissions"}) {
            server.Post(std::string(prefix) + R"(tree/([A-Za-z0-9_-]+)/)" + action,
                [dispatch, action](const auto& req, auto& res) { dispatch(req, res, std::string("tree/") + action); });
        }
        const auto relayed = std::string(prefix) + R"(peers/([A-Za-z0-9_-]+)(/.*))";
        const auto relay_route = [this, listener](const httplib::Request& req, httplib::Response& res) {
            const auto access = nexus_access::route_access(listener, local_request(req), "relay",
                req.get_header_value("Authorization"), api_key(), admin_api_key());
            if (access.first != 0) { reply_error(res, access.first, access.second); return; }
            relay(req, res);
        };
        server.Get(relayed, relay_route);
        server.Post(relayed, relay_route);
    }
}
}

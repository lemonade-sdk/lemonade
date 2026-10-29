#include "lemon/nexus_manager.h"
#include "lemon/error_types.h"
#include "lemon/nexus_user_key.h"
#include "lemon/server/route_registry.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"
#include "lemon/version.h"
#include <lemon/utils/aixlog.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#endif
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
    NexusError(int status, const std::string& message, bool retryable = false)
        : std::runtime_error(message), status(status), retryable(retryable) {}
    int status;
    bool retryable;  // the federation was not reached; the same request may succeed later
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

// Bridges a streamed peer reply to the client. The peer request runs on its own thread
// so the route handler learns the peer's status before httplib commits the response:
// a refusal before the first body byte keeps its status and JSON body, and a failure
// after that becomes one SSE error event. One chunk is in flight at a time, so a slow
// client slows the transfer instead of buffering it.
class RelayStream {
public:
    ~RelayStream() { abandon(); }
    void start(const std::string& url, const std::string& body, const std::map<std::string, std::string>& headers) {
        worker_ = std::thread([this, url, body, headers] { run(url, body, headers); });
    }
    // Blocks until the peer answered or the request failed. Returns the peer's status,
    // or 0 when no answer arrived; `reply` then holds the body of a non-2xx answer.
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
    // Writes the next chunk to the client; after the peer finished, the error event when
    // the stream broke, then the end of the stream. Returns false once the client is gone.
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
                [this](const char* data, size_t length) { return push(data, length); }, headers, 300,
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
    return tree_path(id, std::vector<std::string>{root_id}, fetch);
}

json tree_path(const std::string& id, const std::vector<std::string>& root_ids,
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
        if (std::find(root_ids.begin(), root_ids.end(), current) != root_ids.end()) return path;
        current = text(node, "parent_id");
    }
    throw NexusError(403, "That node is outside your group or its hierarchy is too deep");
}

const std::vector<std::string> kFamilies = {"inference", "mcp", "models_manage", "backends", "jobs", "internal", "system", "management"};

bool service_permission(const std::string& name) {
    static const std::string prefix = "svc:";
    return name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0 &&
        valid_route_family(name.substr(prefix.size()));
}

json role_permissions(const std::string& role) {
    const auto with_families = [](json names, std::initializer_list<const char*> families) {
        for (const char* family : families) names.push_back(std::string("svc:") + family);
        return names;
    };
    if (role == "administrator" || role == "admin") {
        json names = json::array({"read", "connect_private", "admin"});
        for (const auto& family : kFamilies) names.push_back("svc:" + family);
        return ordered(names);
    }
    if (role == "moderator") {
        return ordered(with_families(json::array({"read", "write", "add_child", "connect_private"}),
            {"inference", "mcp", "models_manage", "jobs"}));
    }
    if (role == "user" || role == "member") {
        return ordered(with_families(json::array({"read", "connect_private"}), {"inference"}));
    }
    throw NexusError(400, "Choose the administrator, moderator or user role");
}

json invite_permissions(const json& requested) {
    if (requested.is_null()) return nullptr;
    // The federation refuses any other tree name in an invite; svc: names pass through
    // and the federation reduces them to what the inviter holds.
    static const std::vector<std::string> grantable = {"read", "write", "add_child", "connect_private", "connect_shared"};
    if (!requested.is_array() || requested.empty()) throw NexusError(400, "Send permissions as a list of names");
    json result = json::array();
    for (const auto& name : requested) {
        if (!name.is_string()) throw NexusError(400, "Send permissions as a list of names");
        const auto value = name.get<std::string>();
        if (value == "admin") throw NexusError(400, "An invite cannot grant admin. Change the member's role after it joins.");
        if (std::find(grantable.begin(), grantable.end(), value) == grantable.end() && !service_permission(value)) {
            throw NexusError(400, "An invite cannot grant this permission: " + value);
        }
        if (!contains(result, value)) result.push_back(value);
    }
    return ordered(result);
}

const unsigned kCatalogRevision = 0;

uint64_t catalog_version(const std::string& lemond_version) {
    uint64_t parts[3] = {0, 0, 0};
    size_t pos = 0;
    for (auto& part : parts) {
        const auto start = pos;
        while (pos < lemond_version.size() && lemond_version[pos] >= '0' && lemond_version[pos] <= '9' && pos - start < 6) {
            part = part * 10 + static_cast<uint64_t>(lemond_version[pos] - '0');
            ++pos;
        }
        if (pos == start) return 0;
        if (&part != &parts[2]) {
            if (pos >= lemond_version.size() || lemond_version[pos] != '.') return 0;
            ++pos;
        }
    }
    if (parts[1] > 999 || parts[2] > 999) return 0;
    return ((parts[0] * 1000 + parts[1]) * 1000 + parts[2]) * 100 + kCatalogRevision;
}

namespace {
// One catalog claim of a registered route: the URL as the SDK compares it. An exact
// path loses a trailing slash (except "/"); a {parameter} path becomes the prefix
// before it. `raw` keeps the URL as registered.
struct CatalogClaim {
    bool exact;
    std::string url;
    std::string raw;
};
// Calls `visit(family, id, method, claim)` for every method and URL of every route,
// after validating the family. Throws std::logic_error for a route without a family.
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
    // family, so every claim appears once: repeats of a prefix (several {id} routes
    // under one collection) or of a path collapse, and an exact path that equals a
    // prefix of the same family and method is already covered by that prefix.
    std::set<std::string> prefixes;  // "family METHOD prefix"
    for_each_catalog_claim(routes, [&](const std::string& family, const std::string&, const std::string& method, const CatalogClaim& claim) {
        if (!claim.exact) prefixes.insert(family + " " + method + " " + claim.url);
    });
    nlohmann::ordered_json families = nlohmann::ordered_json::array();
    std::map<std::string, std::string> claims;  // "METHOD path|prefix url", as the SDK keys them -> family
    // The methods of one route and URL form one catalog entry, written when the next
    // URL starts.
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
                {current_key.compare(0, 5, "path ") == 0 ? "path" : "prefix", current_key.substr(current_key.find(' ') + 1)}});
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

const std::vector<std::string> kPeerHeaders = {"X-Nexus-Peer-Node", "X-Nexus-Peer-Key", "X-Nexus-Peer-Permissions",
    "X-Nexus-Family", "X-Nexus-Catalog-Version"};

namespace {
thread_local PeerContext g_request_peer;
}

void accept_peer_headers(httplib::Request& req, bool mesh_ingress) {
    g_request_peer = PeerContext{};
    if (mesh_ingress) {
        g_request_peer.node_id = req.get_header_value("X-Nexus-Peer-Node");
        g_request_peer.family = req.get_header_value("X-Nexus-Family");
        return;
    }
    for (const auto& name : kPeerHeaders) req.headers.erase(name);
}

const PeerContext& request_peer() {
    return g_request_peer;
}

bool invite_code(const std::string& code) {
    const bool full_token = code.size() == 68 && code.rfind("lnk_", 0) == 0 &&
        code.substr(4).find_first_not_of("0123456789abcdef") == std::string::npos;
    return full_token || (code.size() == 8 && code.find_first_not_of("0123456789") == std::string::npos);
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
        {"can_manage", group && admin && access_known}, {"access_known", access_known}, {"is_admin", admin},
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

json group_permissions(const json& requested, bool admin) {
    if (!requested.is_array()) throw NexusError(400, "Send permissions as a list of names");
    json result = json::array({"read"});
    for (const auto& name : requested) {
        if (!name.is_string()) throw NexusError(400, "Send permissions as a list of names");
        const auto value = name.get<std::string>();
        if (value == "admin") throw NexusError(400, "Change the member's role to grant or remove admin");
        if (std::find(kPermissions.begin(), kPermissions.end(), value) == kPermissions.end() && !service_permission(value)) {
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
    if (key.rfind("ed25519:", 0) == 0) key.erase(0, 8);
    std::replace(key.begin(), key.end(), '+', '-');
    std::replace(key.begin(), key.end(), '/', '_');
    while (!key.empty() && key.back() == '=') key.pop_back();
    if (key.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos) return "";
    return key.substr(0, 16);
}

json account_for_key(const json& previous, const json& archived, const std::string& pubkey) {
    json next = archived.is_object() ? archived : json::object();
    for (const char* name : {"server", "genesis_pubkey", "tier1_epoch", "device_name"}) {
        if (!next.contains(name) && previous.is_object() && previous.contains(name)) next[name] = previous.at(name);
    }
    next["public_key"] = pubkey;
    return next;
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
    for (const auto& name : role_permissions("user")) {
        if (admin || grants(held, name.get<std::string>())) granted.push_back(name);
        else dropped.push_back(name);
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

bool remote_operation(const std::string& operation) {
    static const std::vector<std::string> operations = {"status", "join/status", "devices", "members",
        "members/role", "members/permissions", "devices/access", "devices/remove", "invites", "invites/cancel", "join", "relay",
        "selection", "device/check", "tree", "tree/children", "tree/groups", "tree/rename", "tree/permissions", "tree/invites",
        "tree/move", "invites/accept", "memberships", "memberships/leave"};
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
    if (!target.value("is_online", false)) return {409, "That device is offline or not reachable from this device"};
    auto address = target.value("tunnel_ip", "");
    if (const auto slash = address.find('/'); slash != std::string::npos) address.resize(slash);
    if (address.empty()) return {409, "That device has no mesh address"};
    return {0, address};
}

void relay_http(uint16_t port, const std::string& path, const httplib::Request& req, httplib::Response& res,
                const std::string& peer_authorization, const std::string& request_body) {
    const auto url = "http://127.0.0.1:" + std::to_string(port) + path;
    std::map<std::string, std::string> headers;
    if (!peer_authorization.empty()) headers["Authorization"] = peer_authorization;
    if (req.has_header("X-Client-Session-Id")) headers["X-Client-Session-Id"] = req.get_header_value("X-Client-Session-Id");
    res.set_header("Cache-Control", "no-store");
    try {
        if (req.method == "GET") {
            const auto reply = utils::HttpClient::get(url, headers, 0, utils::HttpSecurityPolicy::TrustedLoopback);
            res.status = reply.status_code;
            res.set_content(reply.body, "application/json");
            return;
        }
        auto body = request_body;
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
            auto stream = std::make_shared<RelayStream>();
            stream->start(url, body, headers);
            std::string reply;
            std::string type;
            const auto status = stream->wait_for_status(reply, type);
            if (status == 0) { reply_error(res, 502, "Cannot reach that device's server through the mesh"); return; }
            if (status < 200 || status >= 300) {
                res.status = status;
                res.set_content(reply, type.empty() ? "application/json" : type);
                return;
            }
            // Like streamed backend responses, writes block on the client socket.
            res.set_chunked_content_provider(type.empty() ? "text/event-stream" : type,
                [stream](size_t, httplib::DataSink& sink) { return stream->pull(sink); },
                [stream](bool) { stream->abandon(); });
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
}
SdkFailure sdk_failure(int code, const json& reply, int rejected_status) {
    const auto fallback = "Network request failed (SDK error " + std::to_string(code) + ")";
    const auto message = reply.is_object() ? reply.value("error", fallback) : fallback;
    // The SDK answers LN_ERR_CONNECT for no transport, a timeout and a 5xx. Neither that
    // nor an unreachable federation refuses this device.
    if (code == LN_ERR_CONNECT || federation_unreachable(message)) return {503, message, true};
    const int status = code == LN_ERR_AUTH ? 403 : code == LN_ERR_NOT_FOUND ? 404 : code == LN_ERR_REJECTED ? rejected_status : 502;
    return {status, message, false};
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
// `rejected_status` is the HTTP status for LN_ERR_REJECTED (the federation refused the
// request as invalid for the current state).
json sdk_call(const std::function<ln_error_t(char**)>& operation, int rejected_status = 502) {
    char* output = nullptr;
    const auto code = operation(&output);
    std::unique_ptr<char, decltype(&ln_free)> owned(output, ln_free);
    auto value = output ? json::parse(output, nullptr, false) : json::object();
    if (value.is_discarded()) throw NexusError(502, "The network returned invalid data");
    if (code != LN_OK) {
        const auto failure = nexus_access::sdk_failure(code, value, rejected_status);
        throw NexusError(failure.status, failure.message, failure.retryable);
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
    const IdentitySource source;
    // The outstanding hand-off request; a new request replaces it, and an open consumes it.
    std::unique_ptr<ln_handoff_t, decltype(&ln_identity_handoff_destroy)> handoff{nullptr, ln_identity_handoff_destroy};
    json identity_store = nullptr;  // the SDK's user store status at the last load attempt (User only)
    std::chrono::steady_clock::time_point store_retry_at{};
    std::chrono::seconds store_retry_delay{5};
    std::filesystem::path root;
    int ingress_port;
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
    std::chrono::seconds retry_delay{5};
    std::map<std::string, std::string> egress_ips;
    std::string node_id;
    std::string group_id;
    std::string tunnel_ip;
    std::string domain{"lemonade-nexus.io"};  // network DNS domain
    std::string region{"us-west"};            // SEIP region label
    std::string host;                         // bootstrap server: configured, remembered, or discovered
    std::string rp_id;                        // configured, else the network's own value from /api/health
    std::string pubkey;
    std::string detail{"locked"};
    std::string error;
    bool error_retryable{false};  // `error` is a failure to reach the federation, not a refusal
    json devices = json::array();
    std::chrono::system_clock::time_point last_poll{std::chrono::system_clock::now()};
    const nlohmann::ordered_json catalog;  // mesh route catalog for vport 13305, or null
    std::string catalog_error;             // the SDK refused the catalog at the last join

    Session(const std::string& data_root, int port, int mesh_listener_port, nlohmann::ordered_json service_catalog,
            const ProcessRole& role)
        : source(nexus_access::identity_source(role)), root(utils::path_from_utf8(data_root)), ingress_port(port),
          mesh_port(mesh_listener_port), catalog(std::move(service_catalog)) {
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
        if (const auto value = utils::get_environment_variable_utf8("LEMONADE_NEXUS_DOMAIN"); !value.empty()) domain = value;
        if (const auto value = utils::get_environment_variable_utf8("LEMONADE_NEXUS_REGION"); !value.empty()) region = value;
        if (const auto value = utils::get_environment_variable_utf8("LEMONADE_NEXUS_SERVER"); !value.empty()) host = value;
        if (const auto value = utils::get_environment_variable_utf8("LEMONADE_NEXUS_RP_ID"); !value.empty()) rp_id = value;
        int public_port = 9100;
        const auto configured_port = utils::get_environment_variable_utf8("LEMONADE_NEXUS_PORT");
        if (!configured_port.empty()) public_port = std::stoi(configured_port);
        if (public_port < 1 || public_port > 65535) throw NexusError(400, "Invalid Nexus server port");
        // The servers' certificates name their own FQDNs, not the network apex, so
        // without a configured host the bootstrap server comes from DNS discovery.
        // The first discovered name is remembered, so later starts need no network
        // before the SDK's own verified directory takes over.
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
        client.reset(ln_create_tls(host.c_str(), static_cast<uint16_t>(public_port)));
        if (!client) throw NexusError(500, "Cannot create Nexus client");
        // The SDK moves the session when its Tier 1 server leaves; the journal keeps
        // the record. The callback owns the text and runs on an SDK thread.
        ln_set_session_moved_callback(client.get(), [](char* event_json, void*) {
            std::unique_ptr<char, decltype(&ln_free)> owned(event_json, ln_free);
            LOG(INFO, "Nexus") << "Session moved: " << (event_json ? event_json : "") << std::endl;
        }, nullptr);
        // The SDK answers a refused mesh request itself and reports it here, on an SDK
        // thread: one log line per event, no SDK call besides releasing the text.
        ln_set_ingress_denied_callback(client.get(), [](char* event_json, void*) {
            std::unique_ptr<char, decltype(&ln_free)> owned(event_json, ln_free);
            const auto event = event_json ? json::parse(event_json, nullptr, false) : json();
            const auto line = nexus_access::ingress_denied_log_line(event.is_object() ? event : json::object());
            if (event.is_object() && event.value("kind", "") == "denied") LOG(WARNING, "Nexus") << line << std::endl;
            else LOG(INFO, "Nexus") << line << std::endl;
        }, nullptr);
        // Only a system service keeps an identity file of its own. A desktop identity
        // belongs to the signed-in OS user: lemond loads it from the SDK's keystore-backed
        // store when it runs as that user, and otherwise waits for a sealed hand-off.
        if (source == IdentitySource::System) {
            const auto path = utils::path_to_utf8(root / "identity.json");
            IdentityPtr loaded(ln_identity_load(path.c_str()), ln_identity_destroy);
            if (!loaded) {
                if (std::filesystem::exists(root / "identity.json")) throw NexusError(500, "Device identity is invalid; restore identity.json");
                loaded.reset(ln_identity_generate());
                if (!loaded || ln_identity_save(loaded.get(), path.c_str()) != LN_OK) throw NexusError(500, "Cannot save device identity");
            }
            adopt_identity(std::move(loaded));
        } else if (source == IdentitySource::User) {
            load_user_identity();
        }
        // Tree, session and private-API calls go only to Tier 1 servers verified from this key.
        if (const auto genesis = utils::get_environment_variable_utf8("LEMONADE_NEXUS_GENESIS_PUBKEY"); !genesis.empty()) {
            const auto min_epoch = account.value("tier1_epoch", uint64_t{0});
            if (ln_set_genesis_key(client.get(), genesis.c_str(), min_epoch) != LN_OK) {
                throw NexusError(400, "LEMONADE_NEXUS_GENESIS_PUBKEY is not a base64 Ed25519 public key");
            }
            genesis_key_set = true;
        } else if (const auto pinned = account.value("genesis_pubkey", std::string{}); !pinned.empty()) {
            if (ln_set_genesis_key(client.get(), pinned.c_str(), account.value("tier1_epoch", uint64_t{0})) != LN_OK) {
                throw NexusError(500, "Saved Genesis key is invalid; restore account.json");
            }
            genesis_key_set = true;
        }
    }
    ~Session() { if (client) ln_mesh_disable(client.get()); }
    // Makes `next` the device identity. Another key while one is active ends that
    // session first (lock, drop), then the new key takes over with its own account; the
    // same key changes nothing. Nothing is written except the account files.
    void adopt_identity(IdentityPtr next) {
        const auto key = sdk_string(ln_identity_pubkey(next.get()));
        if (key.empty()) throw NexusError(500, "Cannot read the device identity");
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
        switch_account(key);
        const auto avatar_cache = utils::path_to_utf8(root / "avatar-cache");
        avatar_configured = ln_account_avatar_configure(client.get(), identity.get(), avatar_cache.c_str()) == LN_OK;
        clear_error();
        LOG(INFO, "Nexus") << "Device identity in use: " << pubkey << " (" << nexus_access::identity_source_name(source) << ")" << std::endl;
    }
    // User mode: the identity comes from the SDK's per-user store, created on first use.
    // A store that is locked, refused or unsupported leaves the device without an
    // identity; there is no plaintext fallback. The next poll tries again with backoff.
    void load_user_identity() {
        ln_error_t code = LN_OK;
        IdentityPtr loaded(ln_identity_user_load(&code), ln_identity_destroy);
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
            LOG(WARNING, "Nexus") << error << " (backend " << identity_store.value("backend", "unknown") << ")" << std::endl;
            store_retry_at = std::chrono::steady_clock::now() + store_retry_delay;
            store_retry_delay = std::min(store_retry_delay * 2, std::chrono::seconds(60));
            return;
        }
        store_retry_delay = std::chrono::seconds(5);
        adopt_identity(std::move(loaded));
    }
    void require_identity() const {
        if (identity) return;
        if (source == IdentitySource::Handoff) {
            throw NexusError(409, "Waiting for the signed-in user's device key. Unlock Nexus from the Lemonade tray.");
        }
        throw NexusError(409, error.empty() ? "This device has no identity yet" : error);
    }
    // account.json belongs to the identity in use. When another key takes over, the
    // current account waits as account-<tag>.json and that key's own account, if it was
    // archived before, comes back; otherwise it starts fresh with the network facts.
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
    // Archives the active account as `archive_name` and continues with `key`'s own
    // account: its archived file when one exists, otherwise a fresh account that keeps
    // the network and machine facts.
    void replace_account(const std::string& archive_name, const std::string& key) {
        const auto restore_path = root / ("account-" + nexus_access::account_file_tag(key) + ".json");
        json archived;
        if (std::ifstream input(restore_path); input) {
            archived = json::parse(input, nullptr, false);
            if (!archived.is_object()) throw NexusError(500, "Archived device account is invalid; restore " + restore_path.filename().string());
        }
        write_json(root / archive_name, account);
        account = nexus_access::account_for_key(account, archived, key);
        persist();
        std::error_code ec;
        std::filesystem::remove(restore_path, ec);
        LOG(INFO, "Nexus") << "Archived the account of another identity as " << archive_name << std::endl;
    }
    // Until the user names the device, it carries the machine's host name, as
    // people already know it from their other tools.
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
        if (name.size() > 128) name.resize(128);
        return name.empty() ? "My Lemonade device" : name;
    }
    std::string device_name() const {
        const auto named = account.value("device_name", std::string{});
        return named.empty() ? machine_name() : named;
    }
    void persist() { write_json(root / "account.json", account); }
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
        detail = "locked";
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
    // A failure leaves the session as it is; status reports it until the next success.
    void note_failure(const std::exception& e) {
        set_error(e);
        detail = "degraded";
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
    // A device unlocks with a passkey or with its device key, never both. The device
    // key is the Ed25519 identity, which already authenticates every federation
    // session; key unlock removes the user-presence step, so it also unlocks again by
    // itself after a restart or, on a desktop, after the user's key arrived again.
    bool key_unlock() const { return account.value("unlock", std::string{}) == "device_key"; }
    void unlock_with_key(bool defer_join) {
        require_identity();
        if (account.contains("credential_id")) throw NexusError(409, "This device unlocks with its passkey");
        authenticate_identity();
        if (!key_unlock()) {
            account["unlock"] = "device_key";
            persist();
        }
        hold_locked = false;
        locked = false;
        last_poll = std::chrono::system_clock::now();
        detail = joined ? "running" : "ready";
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
        const auto fetch = [&](const std::string& current) { return private_call("GET", "/api/tree/node/" + current); };
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
    // An empty target selects this account's own top-level group.
    // `held` and `admin` are the inviter's own permissions on the target node.
    json create_invite(const std::string& target, const json& body, const json& held, bool admin) {
        const auto ttl = body.value("ttl_sec", 600);
        if (ttl < 60 || ttl > 600) throw NexusError(400, "Invite lifetime must be 60 to 600 seconds");
        json requested = body.contains("permissions") ? body["permissions"] : json();
        if (body.contains("role")) {
            // A role expands like members/{id}/role; administrator carries admin, which
            // invite_permissions refuses.
            if (!requested.is_null()) throw NexusError(400, "Send a role or a list of permissions, not both");
            if (!body["role"].is_string()) throw NexusError(400, "Send role as a string");
            requested = nexus_access::role_permissions(body["role"].get<std::string>());
        } else if (requested.is_null()) {
            // The federation's own default predates route families and would leave the
            // new device without svc:inference, so the user preset is the default here.
            auto defaults = nexus_access::default_invite_permissions(held, admin);
            if (!defaults.second.empty()) {
                LOG(INFO, "Nexus") << "Invite grants the user preset without " << defaults.second.dump()
                                   << ": this device does not hold them on the target group" << std::endl;
            }
            requested = std::move(defaults.first);
        }
        const auto permissions = nexus_access::invite_permissions(requested);
        const auto list = permissions.is_null() ? std::string() : permissions.dump();
        return sdk_call([&](auto out) {
            return ln_invite_create(client.get(), target.empty() ? nullptr : target.c_str(),
                list.empty() ? nullptr : list.c_str(), static_cast<uint32_t>(ttl), out);
        }, 409);
    }
    // After the federation deletes the account, the local identity has no use. Remove the
    // files the SDK leaves to the caller; preferences.json stays with NexusManager.
    void remove_local_account() {
        lock();
        std::error_code ec;
        for (const auto* name : {"identity.json", "account.json", "account.json.tmp"}) std::filesystem::remove(root / name, ec);
        std::filesystem::remove_all(root / "avatar-cache", ec);
        std::filesystem::remove_all(root / "certs", ec);
        account = json::object();
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
    json mesh_status() {
        try { return sdk_call([&](auto out) { return ln_mesh_status(client.get(), out); }); }
        catch (const std::exception&) { return json::object(); }
    }
    json status() {
        auto mesh = mesh_status();
        json result = {{"status", detail}, {"locked", locked}, {"mesh_up", !locked && ln_mesh_is_active(client.get()) != 0},
            {"node_id", node_id}, {"group_node_id", group_id}, {"tunnel_ip", tunnel_ip},
            {"peer_count", mesh.value("peer_count", 0)}, {"device_name", device_name()}, {"server", host},
            {"registered", account.contains("credential_id") || key_unlock()},
            {"unlock", key_unlock() ? "device_key" : "passkey"}, {"members", devices},
            {"exposed", json::array({{{"vport", 13305}, {"local_port", ingress_port}}})}, {"error", error},
            {"error_retryable", error_retryable}, {"genesis_key_set", genesis_key_set}, {"mesh_port", mesh_port_status()},
            {"catalog_error", catalog_error}, {"identity_source", nexus_access::identity_source_name(source)},
            {"needs_user_key", !identity}, {"public_key", pubkey}};
        if (source == IdentitySource::User) result["identity_store"] = identity_store;
        return result;
    }
    // Receiver side of the SDK hand-off: the request names an ephemeral key that opens
    // one envelope. A new request replaces an unanswered one.
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
        // The user's session delivered the key to unlock with. If the federation cannot
        // be reached right now, the supervisor retries the key unlock by itself.
        if (!account.contains("credential_id") && !key_unlock()) {
            account["unlock"] = "device_key";
            persist();
        }
    }
    json mesh_port_status() {
        json port = sdk_call([&](auto out) { return ln_mesh_port_status(client.get(), out); });
        if (!mesh_port_error.empty()) port["error"] = mesh_port_error;
        return port;
    }
    // The SDK keeps the catalog with the mesh session, so every join registers it
    // again. Without a catalog the SDK bridges the port raw; status says so.
    void register_catalog() {
        catalog_error.clear();
        if (!catalog.is_object()) return;
        const auto text = catalog.dump();
        const auto code = ln_mesh_service_set_catalog(client.get(), 13305, text.c_str());
        if (code != LN_OK) {
            catalog_error = "The SDK refused the route catalog (SDK error " + std::to_string(code) + ")";
            LOG(WARNING, "Nexus") << catalog_error << std::endl;
            return;
        }
        LOG(INFO, "Nexus") << "Registered route catalog version " << catalog.value("version", uint64_t{0})
                           << " with " << catalog.value("families", nlohmann::ordered_json::array()).size()
                           << " families" << std::endl;
    }
    // The SDK owns the mesh port; lemond is its local service through the mesh ingress
    // listener, which is always on loopback and applies lemond's API-key policy.
    void start_mesh_port() {
        mesh_port_error.clear();
        const auto cert_dir = utils::path_to_utf8(root / "certs" / "group-loopback");
        std::error_code ec;
        std::filesystem::create_directories(root / "certs" / "group-loopback", ec);
        ln_mesh_port_set_cert_dir(client.get(), cert_dir.c_str());
        const auto local = "http://127.0.0.1:" + std::to_string(ingress_port);
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
            const auto code = ln_mesh_port_set_selection(client.get(), target.empty() ? nullptr : target.c_str(),
                key.empty() ? nullptr : key.c_str());
            if (code == LN_ERR_NOT_FOUND) throw NexusError(404, "That device is not in your group");
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
        detail = "joining";
        ln_set_link_token(client.get(), code.c_str());
        if (ln_set_inference_node(client.get(), 1) != LN_OK) throw NexusError(500, "Cannot join as an inference node");
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
        if (ln_mesh_enable(client.get()) != LN_OK || ln_mesh_expose_service(client.get(), 13305,
                ("tcp:127.0.0.1:" + std::to_string(ingress_port)).c_str()) != LN_OK) {
            lock();
            throw NexusError(502, "Could not start the mesh transport");
        }
        joined = true;
        reconnect = true;
        retry_delay = std::chrono::seconds(5);
        detail = "running";
        clear_error();
        register_catalog();
        start_mesh_port();
        remember_tier1_epoch();
        try {
            const auto updates = json{{"hostname", device_name()}}.dump();
            sdk_call([&](auto out) { return ln_update_node(client.get(), node_id.c_str(), updates.c_str(), out); });
            refresh_devices();
        } catch (const std::exception& e) { set_error(e); }
    }
    // Keeps the verified Tier 1 epoch so a restart refuses an older membership list. The
    // SDK reports its last verified membership without network I/O, so this runs after
    // every join and every successful poll and writes only when the epoch advanced.
    void remember_tier1_epoch() {
        try {
            const auto membership = sdk_call([&](auto out) { return ln_tier1_membership(client.get(), out); });
            const auto epoch = nexus_access::newer_tier1_epoch(membership, account.value("tier1_epoch", uint64_t{0}));
            if (epoch != 0) {
                account["tier1_epoch"] = epoch;
                persist();
            }
        } catch (const std::exception&) {}
    }
    void poll() {
        const auto now = std::chrono::system_clock::now();
        const auto elapsed = now - last_poll;
        last_poll = now;
        if (!locked && elapsed > std::chrono::minutes(5)) lock();
        if (!identity) {
            if (source == IdentitySource::User && std::chrono::steady_clock::now() >= store_retry_at) load_user_identity();
            if (!identity) return;
        }
        if (locked) {
            if (!key_unlock() || hold_locked || std::chrono::steady_clock::now() < retry_at) return;
            try { unlock_with_key(false); }
            catch (const std::exception& e) {
                note_failure(e);
                retry_at = std::chrono::steady_clock::now() + retry_delay;
                retry_delay = std::min(retry_delay * 2, std::chrono::seconds(60));
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
                detail = "running";
            }
        } catch (const std::exception& e) {
            note_failure(e);
            // Drop a stale session so the next poll authenticates with the device
            // identity through a current server. A refusal while the SDK moves the
            // session by itself is an ordinary failure: the next poll finds it moved.
            if (nexus_access::session_dropped(mesh_status())) {
                ln_mesh_disable(client.get());
                ln_set_session_token(client.get(), "");
                egress_ips.clear();
                joined = false;
                retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                return;
            }
            retry_at = std::chrono::steady_clock::now() + retry_delay;
            retry_delay = std::min(retry_delay * 2, std::chrono::seconds(60));
        }
    }
    json request(const std::string& method, const std::string& path, const json& body) {
        try { return dispatch(method, path, body); }
        catch (const NexusError& e) {
            // A federation that cannot be reached is not a refusal: the device stays
            // unlocked and joined, and status reports the outage until the next success.
            if (e.retryable && !locked) note_failure(e);
            throw;
        }
    }
    json dispatch(const std::string& method, const std::string& path, const json& body) {
        if (path == "/status") return status();
        if (path == "/selection") return selection(method, body);
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
            if (rp_id.empty()) {
                // The RP ID is the network's, not this host's: WebAuthn binds the credential to it.
                const auto health = sdk_call([&](auto out) { return ln_health(client.get(), out); });
                rp_id = health.value("rp_id", std::string{});
                if (rp_id.empty()) throw NexusError(502, "The network did not report its passkey RP ID");
            }
            return {{"challenge", challenge.at("challenge")}, {"user_id", user_id}, {"rp_id", rp_id},
                {"credential_id", account.value("credential_id", "")}, {"registration", path.find("register=1") != std::string::npos}};
        }
        if (path == "/passkey/register") {
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
        if (path == "/key/handoff") return begin_handoff();
        if (path == "/key/unlock") {
            if (body.contains("identity_envelope")) open_handoff(body.at("identity_envelope"));
            unlock_with_key(body.value("defer_join", false));
            return status();
        }
        if (path == "/unlock") {
            if (key_unlock()) throw NexusError(409, "This device unlocks with its device key");
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
        if (path == "/lock") { hold_locked = true; lock(); return status(); }
        if (path == "/link-token/cancel") return {{"cancelled", true}, {"revoked", false}};
        if (locked) throw NexusError(423, "Unlock your network first");
        if (path == "/device/check") {
            // Advisory: tells the UI whether a device name folds to a free DNS label in
            // the target group. The federation still refuses a collision on join or
            // rename, so two devices checking the same name at once cannot both win.
            const auto name = body.value("name", std::string{});
            if (name.empty() || name.size() > 128) throw NexusError(400, "Enter a device name (up to 128 characters)");
            const auto code = body.value("code", std::string{});
            if (joined && code.empty()) {
                return sdk_call([&](auto out) { return ln_check_device_name(client.get(), name.c_str(), group_id.c_str(), "", out); });
            }
            if (!nexus_access::invite_code(code)) throw NexusError(400, "Enter the invite code for the group you are joining");
            return sdk_call([&](auto out) { return ln_check_device_name(client.get(), name.c_str(), "", code.c_str(), out); });
        }
        if (path == "/join") {
            const auto code = body.at("code").get<std::string>();
            if (!nexus_access::invite_code(code)) throw NexusError(400, "Enter an 8-digit code or a full invite token");
            if (joined) throw NexusError(409, "This device already joined. Accept the invite to join another network.");
            join(code);
            return status();
        }
        if (!joined) throw NexusError(409, "Join the network first");
        if (path == "/invites/accept") {
            const auto code = body.at("code").get<std::string>();
            if (!nexus_access::invite_code(code)) throw NexusError(400, "Enter an 8-digit code or a full invite token");
            auto result = sdk_call([&](auto out) { return ln_invite_accept(client.get(), code.c_str(), out); }, 409);
            refresh_devices();
            return result;
        }
        if (path == "/memberships") {
            auto result = memberships();
            result["group_node_id"] = group_id;
            return result;
        }
        if (path.rfind("/memberships/", 0) == 0) {
            const auto separator = path.find('/', 13);
            if (separator == std::string::npos || path.substr(separator) != "/leave") throw NexusError(404, "Unknown membership operation");
            const auto id = path.substr(13, separator - 13);
            if (id == group_id) throw NexusError(403, "This device's own group cannot be left. Remove the device instead.");
            sdk_call([&](auto out) { return ln_membership_leave(client.get(), id.c_str(), out); }, 409);
            return {{"left", true}, {"node_id", id}};
        }
        if (path == "/account/delete" && method == "POST") {
            if (body.value("account_id", "") != group_id || body.value("member_id", "") != account.value("user_id", "") ||
                body.value("device_node_id", "") != node_id) throw NexusError(409, "The account changed. Sign in again.");
            if (body.value("confirm", "") != "delete") throw NexusError(400, "Send \"confirm\": \"delete\" to delete this account permanently");
            auto result = sdk_call([&](auto out) { return ln_account_delete(client.get(), out); }, 409);
            if (text(result, "status") == "deleted") remove_local_account();
            return result;
        }
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
            if (method == "POST" && action == "move") {
                const auto parent_id = body.at("parent_id").get<std::string>();
                if (parent_id == id) throw NexusError(400, "A node cannot move under itself");
                const auto destination = describe_tree_node(scoped_tree_path(parent_id));
                if (destination.at("kind") != "group") throw NexusError(400, "Move a device or group into a group");
                if (!destination.at("can_add_group").get<bool>()) throw NexusError(403, "You do not have permission to add to that group");
                auto result = sdk_call([&](auto out) { return ln_move_node(client.get(), id.c_str(), parent_id.c_str(), out); }, 409);
                refresh_devices();
                return result;
            }
            if (target.at("kind") != "group") throw NexusError(400, "Select a group for this action");
            if (method == "POST" && action == "invites") {
                if (!target.at("can_add_group").get<bool>()) throw NexusError(403, "You do not have permission to invite members to this group");
                const auto name = body.value("device_name", std::string());
                if (name.size() > 128) throw NexusError(400, "Enter a name of up to 128 characters");
                auto result = create_invite(id, body, target.at("permissions"), target.at("is_admin").get<bool>());
                if (!name.empty()) result["device_name"] = name;
                return result;
            }
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
            const auto name = body.at("device_name").get<std::string>();
            if (name.empty() || name.size() > 128) throw NexusError(400, "Enter the name of the device you're inviting");
            const auto self = find_member(devices, node_id);
            auto result = create_invite("", body, self.is_object() ? self.value("permissions", json::array()) : json::array(),
                self.is_object() && self.value("is_admin", false));
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
                permissions = nexus_access::role_permissions(body.at("role").get<std::string>());
                if (!contains(permissions, "admin") && target.at("is_owner").get<bool>()) throw NexusError(403, "The group creator always administers the group");
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
                           const std::function<void(httplib::Server&)>& install_api,
                           int mesh_port,
                           nlohmann::ordered_json service_catalog,
                           std::optional<ProcessRole> process_role)
    : data_root_(utils::path_to_utf8(utils::path_from_utf8(config_dir) / "nexus")), catalog_(std::move(service_catalog)),
      role_(process_role ? *process_role : nexus_access::current_process_role()), selection_port_(mesh_port) {
#ifdef LEMONADE_WITH_NEXUS
    if (!catalog_.is_object()) {
        LOG(INFO, "Nexus") << "Route policy is unavailable: no route catalog" << std::endl;
    } else if (!nexus_access::route_policy_available()) {
        LOG(INFO, "Nexus") << "Route policy is unavailable: the Nexus SDK does not export ln_mesh_service_set_catalog" << std::endl;
    }
#endif
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
    const bool catalog = catalog_.is_object();
    result["catalog_version"] = catalog ? catalog_.value("version", uint64_t{0}) : uint64_t{0};
    result["app"] = catalog ? json(catalog_.at("app")) : json{{"name", "lemonade"}, {"version", LEMON_VERSION_STRING}};
    bool enforced = false;
#ifdef LEMONADE_WITH_NEXUS
    enforced = catalog && nexus_access::route_policy_available() && snapshot_.value("catalog_error", std::string()).empty();
#endif
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
    std::lock_guard<std::mutex> session_lock(session_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
#ifndef LEMONADE_WITH_NEXUS
    error = "This build does not include the Nexus SDK";
    return false;
#else
    try {
        if (stopping_) throw std::runtime_error("Server is shutting down");
        if (!session_) session_ = std::make_unique<Session>(data_root_, ingress_port_, selection_port_, catalog_, role_);
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
        else if (operation == "memberships/leave") path = "/memberships/" + req.matches[1].str() + "/leave";
        else if (operation.rfind("tree/", 0) == 0) path = "/tree/" + req.matches[1].str() + "/" + operation.substr(5);
        else path = "/" + operation;
        auto result = control(req.method, path, body);
        // The federation deleted the account and the session removed its local files.
        if (operation == "account/delete" && result.first == 200 && result.second.value("status", "") == "deleted") disable();
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
    nexus_access::relay_http(port, path, req, res, req.get_header_value(nexus_access::kPeerAuthorizationHeader), req.body);
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
        for (const auto* operation : {"status", "passkey/challenge", "key/handoff", "devices", "members", "join/status", "events", "tree", "account", "selection", "memberships"}) {
            server.Get(std::string(prefix) + operation, [dispatch, operation](const auto& req, auto& res) { dispatch(req, res, operation); });
        }
        for (const auto* operation : {"enable", "disable", "passkey/register", "passkey/unlock", "key/unlock", "invites", "invites/cancel", "join", "egress", "lock", "device", "device/check", "account/avatar", "selection", "invites/accept", "account/delete"}) {
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
        server.Post(std::string(prefix) + R"(memberships/([A-Za-z0-9_-]+)/leave)",
            [dispatch](const auto& req, auto& res) { dispatch(req, res, "memberships/leave"); });
        server.Get(std::string(prefix) + R"(tree/([A-Za-z0-9_-]+)/children)",
            [dispatch](const auto& req, auto& res) { dispatch(req, res, "tree/children"); });
        for (const auto* action : {"groups", "rename", "permissions", "invites", "move"}) {
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

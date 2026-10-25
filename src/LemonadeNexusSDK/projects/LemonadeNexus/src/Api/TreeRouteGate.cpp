#include <LemonadeNexus/Api/TreeRouteGate.hpp>

#include <nlohmann/json.hpp>

#include <array>

namespace nexus::api {

namespace {

/// A prefix matches the path exactly or up to a '/' boundary.
[[nodiscard]] bool under(std::string_view path, std::string_view prefix) {
    if (!path.starts_with(prefix)) return false;
    return path.size() == prefix.size() || path[prefix.size()] == '/';
}

constexpr std::array<std::string_view, 17> kTreeRoutePrefixes = {
    "/api/join",
    "/api/auth",
    "/api/link",
    "/api/tree",
    "/api/ipam",
    "/api/mesh",
    "/api/routing",
    "/api/chats",
    "/api/account/keys",
    "/api/certs",
    // Root-admin routes: the admin permission is a tree permission.
    "/api/relay/register",
    "/api/onboard/pending",
    "/api/onboard/approve",
    "/api/onboard/deny",
    "/api/onboard/token",
    "/api/storage",
    "/api/credentials/unrevoke",
};

}  // namespace

bool is_tree_route(std::string_view path) {
    // A path that is not canonical is refused rather than matched loosely.
    if (path.find("//") != std::string_view::npos || path.find("/..") != std::string_view::npos ||
        path.find("/./") != std::string_view::npos) {
        return path.starts_with("/api/");
    }
    for (const auto prefix : kTreeRoutePrefixes) {
        if (under(path, prefix)) return true;
    }
    return false;
}

void refuse_tree_route(httplib::Response& res) {
    res.headers.clear();
    res.status = kTreeRouteRefusedStatus;
    const nlohmann::json body{{"error", std::string(kTreeRouteRefusedError)}};
    res.set_content(body.dump(), "application/json");
}

bool gate_tree_route(const httplib::Request& req, httplib::Response& res,
                     const std::function<bool()>& holds_tree) {
    if (holds_tree && holds_tree()) return false;
    if (!is_tree_route(req.path)) return false;
    refuse_tree_route(res);
    return true;
}

}  // namespace nexus::api

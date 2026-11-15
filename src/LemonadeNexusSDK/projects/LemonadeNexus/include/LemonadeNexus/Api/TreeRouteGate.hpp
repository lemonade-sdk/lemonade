#pragma once

/// The HTTP gate for every route that reads, writes, or authorizes against the
/// permission tree, or that opens a client tree session.
///
/// Only a current Tier 1 member holds the tree (Tree/TreeRole.hpp). On any
/// other server each of these routes answers 421 Misdirected Request with a
/// fixed error body: no redirect, no Location header, no hint of another
/// server, and no tree data. Clients find Tier 1 servers through the verified
/// membership (GET /api/tier1/authority and /api/tier1/proof), never through
/// this refusal.
///
/// Gated route families (public and private API):
///   /api/join                    tree join
///   /api/auth, /api/auth/...     client sessions
///   /api/link/...                device link tokens (tree group lookup)
///   /api/tree/...                tree reads, writes, op status, log status
///   /api/ipam/...                tunnel allocation for tree endpoints
///   /api/mesh/...                tree-derived peers and heartbeats
///   /api/routing/...             tree-authorized connection brokering
///   /api/chats, /api/account/keys/...  tree-authorized account data
///   /api/certs/...               certificates for tree endpoints
///   root-admin routes, whose admin permission is a tree permission:
///   /api/relay/register, /api/onboard/{pending,approve,deny,token},
///   /api/storage/..., /api/credentials/unrevoke
///
/// A path that is not canonical ("//", "/./", "/..") under /api/ is refused
/// too, so a loose spelling cannot pass the gate.

#include <httplib.h>

#include <functional>
#include <string_view>

namespace nexus::api {

/// The status a refused tree route answers.
inline constexpr int kTreeRouteRefusedStatus = 421;

/// The fixed error text of a refused tree route.
inline constexpr std::string_view kTreeRouteRefusedError =
    "this server does not serve the tree";

/// True when `path` (the request path, without the query) names a gated tree
/// route. Matches whole path segments, so "/api/treehouse" is not a tree
/// route.
[[nodiscard]] bool is_tree_route(std::string_view path);

/// Write the refusal: status kTreeRouteRefusedStatus and
/// {"error":"<kTreeRouteRefusedError>"}. Clears any header already set.
void refuse_tree_route(httplib::Response& res);

/// A pre-routing check: when `holds_tree()` is false and the request names a
/// tree route, write the refusal and return true (handled). Otherwise return
/// false and let routing continue. `holds_tree` must be thread-safe; an empty
/// function counts as false.
[[nodiscard]] bool gate_tree_route(const httplib::Request& req, httplib::Response& res,
                                   const std::function<bool()>& holds_tree);

}  // namespace nexus::api

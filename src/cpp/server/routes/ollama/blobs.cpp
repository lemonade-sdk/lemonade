#include "lemon/server/api_route.h"

namespace lemon {
namespace {

class BlobsRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "ollama.blobs";
        s.methods = {"POST"};
        s.paths = {"/api/blobs/{digest}"};
        s.prefixes = Prefixes::Root;
        s.summary = "Upload a blob (not supported)";
        s.description = "Not supported: answers `501`.";
        s.args = {
            {"digest", ArgIn::Path, {{"type", "string"}, {"pattern", ".+"}}, true, Support::NotAvailable,
             "SHA256 digest of the blob."},
        };
        return s;
    }

    void handle(RouteRequest&, httplib::Response& res) override {
        res.status = 501;
        res.set_content(R"({"error":"not supported by Lemonade"})", "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_ollama_blobs_route(ServerContext& ctx) {
    return std::make_unique<BlobsRoute>(ctx);
}

} // namespace lemon

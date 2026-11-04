#include "unsupported.h"

namespace lemon {
namespace {

const RouteArg kDigest{"digest", ArgIn::Path, {{"type", "string"}, {"pattern", ".+"}}, true, Support::NotAvailable,
                       "SHA256 digest of the blob."};

class BlobsRoute : public OllamaUnsupportedRoute {
public:
    using OllamaUnsupportedRoute::OllamaUnsupportedRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "ollama.blobs";
        s.methods = {"POST"};
        s.paths = {"/api/blobs/{digest}"};
        s.prefixes = Prefixes::Root;
        s.summary = "Upload a blob";
        s.args = {kDigest};
        s.errors = {kNotSupported};
        return s;
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_ollama_blobs_route(ServerContext& ctx) {
    return std::make_unique<BlobsRoute>(ctx);
}

} // namespace lemon

#include "lemon/model_manager.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

class ModelSourcesRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "internal.model_sources";
        s.methods = {"GET"};
        s.paths = {"model-sources"};
        s.prefixes = Prefixes::Internal;
        s.summary = "List every model source's descriptor";
        s.description =
            "Returns each remote registry's and each model catalog's compiled descriptor: its "
            "token, aliases, documentation text, credential environment variables, cache layout "
            "and endpoint defaults.";
        s.notes = {
            "`docs/tools/gen_model_source_boilerplate.py` reads this endpoint to generate the "
            "model-source pages, so a committed page cannot name a token, an endpoint or an "
            "environment variable the running server does not have.",
            "A catalog answers `host_specific: true` when its rows depend on this machine, such "
            "as an extra-models directory or a configured path. The generator never writes such a "
            "row to a committed page, so the page stays the same on every machine even though this "
            "response is not.",
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"json({
            "type": "object",
            "required": ["registries", "catalogs", "source_tokens", "local_origin_tokens",
                         "pull_source_values"],
            "properties": {
                "registries": {"type": "array", "description": "One entry per remote registry the server can download from.", "items": {
                    "type": "object",
                    "required": ["token", "aliases", "display_name", "title", "summary", "notes",
                                 "example", "endpoint_default", "scheme", "cache_dir_prefix",
                                 "default_revision", "revision_semantics", "staleness_basis",
                                 "download_protocol", "search", "env"],
                    "properties": {
                        "token": {"type": "string", "description": "The registry's canonical name, as `source` and a checkpoint URL spell it."},
                        "aliases": {"type": "array", "items": {"type": "string"}, "description": "Other names a request may send for this registry."},
                        "display_name": {"type": "string", "description": "The registry's name in prose and error messages."},
                        "title": {"type": "string", "description": "Heading the generated docs give this registry."},
                        "summary": {"type": "string", "description": "The registry's one-line description."},
                        "notes": {"type": "array", "items": {"type": "string"}, "description": "Anything the generated page says about this registry beyond the table row."},
                        "example": {"type": "string", "description": "A checkpoint the docs show for this registry."},
                        "endpoint_default": {"type": "string", "description": "API endpoint used when no environment variable overrides it."},
                        "scheme": {"type": "string", "description": "URL scheme a checkpoint URL for this registry carries, such as `huggingface://`; empty when a plain URL names it."},
                        "cache_dir_prefix": {"type": "string", "description": "Prefix of this registry's directory under the models cache."},
                        "default_revision": {"type": "string", "description": "Revision assumed when a request names none."},
                        "revision_semantics": {"type": "string", "description": "What a revision means here: an immutable commit, a moving branch, or the registry's own token."},
                        "staleness_basis": {"type": "string", "description": "How the server decides a download is out of date."},
                        "download_protocol": {"type": "string", "description": "How a file is fetched from this registry."},
                        "search": {"type": "boolean", "description": "Whether [`/v1/registry/search`](./lemonade.md#get-v1registrysearch) answers for this registry."},
                        "env": {"type": "array", "description": "Environment variables that configure this registry.", "items": {
                            "type": "object",
                            "required": ["name", "purpose", "required"],
                            "properties": {
                                "name": {"type": "string", "description": "Variable name."},
                                "purpose": {"type": "string", "description": "What it sets."},
                                "required": {"type": "boolean", "description": "Whether the registry is unusable without it."}
                            }
                        }}
                    }
                }},
                "catalogs": {"type": "array", "description": "One entry per model catalog the server reads.", "items": {
                    "type": "object",
                    "required": ["id", "title", "summary", "notes", "example", "required",
                                 "config_key", "host_specific"],
                    "properties": {
                        "id": {"type": "string", "description": "The catalog's id, as `model_catalogs` in config.json names it."},
                        "title": {"type": "string", "description": "Heading the generated docs give this catalog."},
                        "summary": {"type": "string", "description": "The catalog's one-line description."},
                        "notes": {"type": "array", "items": {"type": "string"}, "description": "Anything the generated page says about this catalog beyond the table row."},
                        "example": {"type": "string", "description": "A configuration example the docs show."},
                        "required": {"type": "boolean", "description": "Whether a model set missing this catalog invalidates the set."},
                        "config_key": {"type": "string", "description": "The `model_catalogs` key that configures it; empty when nothing configures it."},
                        "host_specific": {"type": "boolean", "description": "`true` when its rows depend on this machine, which keeps its rows out of committed pages."}
                    }
                }},
                "source_tokens": {"type": "array", "items": {"type": "string"}, "description": "Every registry token, in the order the registries are listed."},
                "local_origin_tokens": {"type": "array", "items": {"type": "string"}, "description": "Tokens naming where a model came from without a registry, such as `local_upload`."},
                "pull_source_values": {"type": "array", "items": {"type": "string"}, "description": "Every value [`/v1/pull`](./lemonade.md#post-v1pull) accepts for `source`: each registry token and each local origin."}
            }
        })json");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = json::object();
        s.examples = {example};
        s.quiet_log = true;
        return s;
    }

    void handle(RouteRequest&, httplib::Response& res) override {
        res.set_content(ctx_.model_manager->model_sources().dump(2), "application/json");
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_model_sources_route(ServerContext& ctx) {
    return std::make_unique<ModelSourcesRoute>(ctx);
}

} // namespace lemon

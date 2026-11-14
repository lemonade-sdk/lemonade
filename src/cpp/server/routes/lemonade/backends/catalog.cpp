#include "lemon/backends/configured/backend_catalog.h"
#include "lemon/server/api_route.h"

#include <sstream>

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kInvalidRepresentation{
    400, "invalid_representation", "`representation` is repeated or is neither `1` nor `2`"};
const RouteError kInvalidRecipeFilter{
    400, "invalid_recipe_filter", "`recipe` is repeated or empty"};
const RouteError kUnknownRecipe{
    404, "unknown_recipe", "`recipe` names no recipe in the catalog"};
const RouteError kInvalidDefinition{
    400, "", "The definition is not valid; the body is the validation report with `valid` set to `false`"};
const RouteError kDefinitionTooLarge{
    413, "", "The body is larger than 1 MiB"};

const RouteArg kRecipeFilter{"recipe", ArgIn::Query, {{"type", "string"}}, false, Support::Available,
                             "Returns only this recipe, such as `llamacpp`, in the same envelope. The match is exact."};
const RouteArg kRepresentation{"representation", ArgIn::Query, {{"type", "string"}}, false, Support::Available,
                               "`1`, the default, returns the catalog of executable backends. `2` adds the admission "
                               "view: the definition versions the server accepts and the identities of its "
                               "admission rules."};

const RouteArg kDefinitionSchemaVersion{"schema_version", ArgIn::JsonBody, {{"type", "integer"}}, true, Support::Available,
                                        "The version of the backend definition format the document is written in."};
const RouteArg kDefinitionId{"id", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
                             "The definition's identifier."};
const RouteArg kDefinitionRecipe{"recipe", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
                                 "The recipe the backend serves."};
const RouteArg kDefinitionKind{"kind", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
                               "The kind of backend, such as `local`."};
const RouteArg kDefinitionMetadata{"metadata", ArgIn::JsonBody, {{"type", "object"}}, true, Support::Available,
                                   "Display name, binary, devices and the options the recipe accepts."};
const RouteArg kDefinitionInstall{"install", ArgIn::JsonBody, {{"type", "object"}}, true, Support::Available,
                                  "How the backend's executable is obtained."};
const RouteArg kDefinitionLaunch{"launch", ArgIn::JsonBody, {{"type", "object"}}, false, Support::Available,
                                 "How the backend process is started, checked for readiness and watched."};
const RouteArg kDefinitionOperations{"operations", ArgIn::JsonBody, {{"type", "array"}}, true, Support::Available,
                                     "The operations the backend serves and how each request reaches it."};

class BackendSchemaRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;
    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.backends_schema";
        s.methods = {"GET"};
        s.paths = {"backends/schema"};
        s.summary = "List backend recipes with their options and support";
        s.description =
            "Returns the server's backend catalog: every recipe with the options it accepts, the "
            "backends it supports per operating system and device, and the operations it serves. "
            "Clients use it to build option forms and to check a request against the server "
            "they are connected to.";
        s.notes = {
            "The response carries an `ETag` and `Cache-Control: private, no-cache`. Send the tag back "
            "in `If-None-Match` to get `304` with no body while the catalog is unchanged. The tag covers "
            "the exact body, so it differs per `recipe` and `representation`.",
            "Executable paths, command lines, environment values, install configuration, current settings "
            "and credentials are never part of the catalog.",
        };
        s.args = {kRecipeFilter, kRepresentation};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["schema_version", "control_version", "definition_schema_version", "catalog_id",
                         "feature_manifest_id", "recipes"],
            "properties": {
                "schema_version": {"type": "integer", "description": "The representation returned: `1` or `2`."},
                "control_version": {"type": "integer", "description": "Version of the option control vocabulary in `ui`."},
                "definition_schema_version": {"type": "integer", "description": "Newest backend definition format this representation describes."},
                "catalog_id": {"type": "string", "description": "SHA-256 identity of the catalog of executable backends. It changes when a definition, its origin or the server's feature set changes."},
                "feature_manifest_id": {"type": "string", "description": "SHA-256 identity of the definition features this server implements."},
                "recipes": {
                    "type": "array",
                    "description": "The recipes, in catalog order.",
                    "items": {
                        "type": "object",
                        "properties": {
                            "recipe": {"type": "string", "description": "The recipe name."},
                            "display_name": {"type": "string", "description": "Name shown to users."},
                            "source": {"type": "string", "description": "`builtin` for a backend shipped with the server, otherwise where the definition was loaded from."},
                            "definition_id": {"type": "string", "description": "Identifier of the definition that serves the recipe; absent for a backend compiled into the server."},
                            "definition_hash": {"type": "string", "description": "SHA-256 of the definition's canonical JSON; absent for a backend compiled into the server."},
                            "input_schema_version": {"type": ["integer", "null"], "description": "`representation=2`: the format version the definition was written in, or `null` for a backend compiled into the server."},
                            "supported_modes": {"type": "array", "items": {"type": "string"}, "description": "Model modes the recipe can load, such as `chat` or `tts`."},
                            "required_checkpoints": {"type": "array", "items": {"type": "string"}, "description": "Checkpoint roles a model of this recipe must provide."},
                            "operations_complete": {"type": "boolean", "description": "`false` when `operations` may be missing entries. It does not mean an operation is unsupported."},
                            "operations": {
                                "type": "array",
                                "description": "The operations the backend serves.",
                                "items": {
                                    "type": "object",
                                    "properties": {
                                        "operation": {"type": "string", "description": "The operation, such as `chat_completion` or `text_to_speech`."},
                                        "delivery": {"type": "string", "description": "`buffered`, `streaming` or `realtime`."},
                                        "model_modes": {"type": "array", "items": {"type": "string"}, "description": "Model modes the operation applies to."},
                                        "provenance": {"type": "string", "description": "`implemented` when the backend serves the operation itself, `legacy_forwarding` when the request is forwarded unchanged."}
                                    }
                                }
                            },
                            "options": {
                                "type": "array",
                                "description": "The options the recipe accepts, shared ones included.",
                                "items": {
                                    "type": "object",
                                    "properties": {
                                        "name": {"type": "string", "description": "The option name, as used in `recipe_options` and [`POST /v1/load`](#post-v1load)."},
                                        "value_type": {"type": "string", "description": "`boolean`, `integer`, `number` or `string`."},
                                        "default_value": {"description": "The default, or `null` when the server decides."},
                                        "unset_values": {"type": "array", "description": "Values that mean the option is not set."},
                                        "group": {"type": "string", "description": "Heading the option is listed under."},
                                        "help": {"type": "string", "description": "One-line description."},
                                        "ui": {"type": "object", "description": "How a client presents the option: `control`, `label`, `order` and, for a select, `choices`."}
                                    }
                                }
                            },
                            "support": {
                                "type": "array",
                                "description": "The backends of the recipe and where each one runs.",
                                "items": {
                                    "type": "object",
                                    "properties": {
                                        "backend": {"type": "string", "description": "The backend, such as `vulkan` or `cpu`."},
                                        "tier": {"type": "string", "description": "`core`, `experimental` or `guest`, as defined by the [backend support tier policy](../dev/adding-a-backend.md#support-tier)."},
                                        "format": {"type": "string", "description": "How the backend is packaged, such as `native` or `python`."},
                                        "supported_os": {"type": "array", "items": {"type": "string"}, "description": "Operating systems the backend runs on."},
                                        "devices": {"type": "object", "description": "Device classes the backend runs on, each with the architectures or families it accepts; an empty list accepts all."},
                                        "device_summary": {"type": "string", "description": "The devices in words."},
                                        "arch_gates": {"type": "object", "description": "Per GPU architecture, the operating systems and release channels it is limited to."}
                                    }
                                }
                            }
                        }
                    }
                },
                "catalog_v2_id": {"type": "string", "description": "`representation=2`: SHA-256 identity of the admission view."},
                "legacy_feature_manifest_id": {"type": "string", "description": "`representation=2`: the `feature_manifest_id` of representation `1`."},
                "accepted_definition_versions": {"type": "array", "items": {"type": "integer"}, "description": "`representation=2`: definition format versions the server admits."},
                "executable_definition_versions": {"type": "array", "items": {"type": "integer"}, "description": "`representation=2`: definition format versions the server can run."},
                "acceptance_stage": {"type": "string", "description": "`representation=2`: the last stage an admitted definition passes."},
                "resolver_revision": {"type": "integer", "description": "`representation=2`: revision of the dependency resolver."},
                "authorizer_revision": {"type": "integer", "description": "`representation=2`: revision of the authorizer."},
                "grant_policy_revision": {"type": "integer", "description": "`representation=2`: revision of the grant policy."},
                "export_registry_id": {"type": "string", "description": "`representation=2`: SHA-256 identity of the export registry."},
                "admission_manifest": {"type": "object", "description": "`representation=2`: the admission rules as one independently versioned record, including the schema and hook identities and the size bounds applied to definitions."}
            }
        })");
        s.responses = {response};

        RouteExample recipe_example;
        recipe_example.name = "recipe";
        recipe_example.format = ResponseFormat::Json;
        recipe_example.args = ordered_json::parse(R"({"recipe": "kokoro"})");

        RouteExample admission_example;
        admission_example.name = "admission";
        admission_example.format = ResponseFormat::Json;
        admission_example.args = ordered_json::parse(R"({"recipe": "kokoro", "representation": "2"})");

        s.examples = {recipe_example, admission_example};
        s.errors = {kInvalidRepresentation, kInvalidRecipeFilter, kUnknownRecipe};
        s.error_format = ErrorFormat::OpenAi;
        return s;
    }
    void handle(RouteRequest& request, httplib::Response& res) override {
        const auto& req = request.http;
        size_t count = 0, representation_count = 0;
        const auto query_start = req.target.find('?');
        if (query_start != std::string::npos) {
            // httplib collapses identical query pairs; the original target preserves them.
            std::istringstream query(req.target.substr(query_start + 1));
            std::string field;
            while (std::getline(query, field, '&')) {
                httplib::Params params;
                httplib::detail::parse_query_text(field, params);
                count += params.count("recipe");
                representation_count += params.count("representation");
            }
        }
        const std::string representation = request.value(kRepresentation, "");
        if (representation_count > 1 || (representation_count && representation != "1" && representation != "2")) {
            write_openai_error(res, kInvalidRepresentation, "Expected one supported catalog representation", "invalid_request_error");
            return;
        }
        const std::string recipe = request.value(kRecipeFilter, "");
        if (count > 1 || (count == 1 && recipe.empty())) {
            write_openai_error(res, kInvalidRecipeFilter, "Expected one nonempty recipe filter", "invalid_request_error");
            return;
        }
        const auto schema = backends::frozen_backend_catalog().export_schema(recipe, representation == "2" ? 2 : 1);
        if (count && schema["recipes"].empty()) {
            write_openai_error(res, kUnknownRecipe, "Unknown backend recipe: " + recipe, "invalid_request_error");
            return;
        }
        const auto etag = "\"" + backends::schema_digest(schema.dump()) + "\"";
        res.set_header("ETag", etag);
        res.set_header("Cache-Control", "private, no-cache");
        if (req.get_header_value("If-None-Match") == etag) { res.status = 304; return; }
        res.set_content(schema.dump(), "application/json");
    }
};

class BackendValidateRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;
    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.backends_validate";
        s.methods = {"POST"};
        s.paths = {"backends/validate"};
        s.summary = "Check a backend definition against this server without installing it";
        s.description =
            "Validates a backend definition document against the server's definition format, its "
            "installed feature set and its catalog. Nothing is written, installed, launched or "
            "published, and a valid answer does not promise that the executable or a model is "
            "available.";
        s.notes = {
            "The body is the definition document itself, read as sent: the parameters below are its "
            "top-level fields. [Runtime backends](../dev/runtime-backends.md) describes the format.",
            "A definition that reuses the `id` and `recipe` of an installed external definition is "
            "checked as its replacement. A built-in recipe cannot be replaced.",
        };
        s.args = {kDefinitionSchemaVersion, kDefinitionId, kDefinitionRecipe, kDefinitionKind,
                  kDefinitionMetadata, kDefinitionInstall, kDefinitionLaunch, kDefinitionOperations};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["valid", "validation", "catalog_id", "diagnostics"],
            "properties": {
                "valid": {"type": "boolean", "description": "Whether the server would accept and run the definition."},
                "validation": {"type": "string", "description": "`server`: the answer comes from this server's installed feature set."},
                "catalog_id": {"type": "string", "description": "The `catalog_id` of [`GET /v1/backends/schema`](#get-v1backendsschema) the definition was checked against."},
                "definition_id": {"type": "string", "description": "The definition's `id`, when it is valid."},
                "recipe": {"type": "string", "description": "The definition's `recipe`, when it is valid."},
                "definition_hash": {"type": "string", "description": "SHA-256 of the definition's canonical JSON, when it is valid. Key order and whitespace do not affect it."},
                "input_schema_version": {"type": "integer", "description": "For a version 2 definition, its `schema_version`."},
                "stages": {"type": "object", "description": "For a version 2 definition, which admission stages it passed: `resolved`, `authorized`, `catalog_admitted` and `executable`."},
                "diagnostics": {
                    "type": "array",
                    "description": "What is wrong with the definition, in order; empty when it is valid.",
                    "items": {
                        "type": "object",
                        "properties": {
                            "code": {"type": "string", "description": "The kind of problem, such as `required` or `syntax_error`."},
                            "message": {"type": "string", "description": "The problem in words."},
                            "pointer": {"type": "string", "description": "JSON pointer to the field at fault."},
                            "definition": {"type": "string", "description": "The `id` of the definition at fault, or `\"\"` when it could not be read."}
                        }
                    }
                }
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = ordered_json::parse(R"({
            "schema_version": 1,
            "id": "example",
            "recipe": "example",
            "kind": "local",
            "metadata": {
                "display_name": "Example",
                "server_name": "example-server",
                "binary": "example-server",
                "default_device": "cpu",
                "bin_variants": ["cpu"],
                "options": [
                    {"name": "example_batch", "value_type": "integer", "default_value": 2, "minimum": -1, "maximum": 8}
                ]
            },
            "install": {"strategy": "external_only"},
            "launch": {
                "prepare_order": ["artifacts", "backend", "program", "ports"],
                "backend": {"fixed": "cpu"},
                "program": {"source": "external_or_managed", "install_policy": "always"},
                "ports": [{"name": "http", "reject_zero": true, "error": "No port"}],
                "argv": [{"args": ["--serve"]}],
                "environment": {"preset": "inherit"},
                "working_directory": "inherit",
                "readiness": {"port": "http", "path": "/health", "timeout": 5},
                "watchdog": {"port": "http", "path": "/health", "policy": "none"},
                "output": {"inherit_levels": ["debug"], "filter": false}
            },
            "operations": [
                {
                    "operation": "chat_completion",
                    "delivery": "buffered",
                    "model_modes": ["chat"],
                    "transport": {"kind": "http_json", "method": "POST", "path": "/chat", "port": "http",
                                  "timeout": 5, "response_codec": "json.v1"},
                    "request": {"base": "identity"}
                }
            ]
        })");
        s.examples = {example};
        s.errors = {kInvalidDefinition, kDefinitionTooLarge};
        return s;
    }
    void handle(RouteRequest& request, httplib::Response& res) override {
        const auto& req = request.http;
        const auto result = backends::frozen_backend_catalog().validate(req.body);
        res.status = req.body.size() > 1024 * 1024 ? 413 : result["valid"].get<bool>() ? 200 : 400;
        res.set_content(result.dump(), "application/json");
    }
};

}

std::unique_ptr<ApiRoute> make_backend_schema_route(ServerContext& ctx) {
    return std::make_unique<BackendSchemaRoute>(ctx);
}

std::unique_ptr<ApiRoute> make_backend_validate_route(ServerContext& ctx) {
    return std::make_unique<BackendValidateRoute>(ctx);
}

}

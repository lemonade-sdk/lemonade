#include <string>

#include "lemon/backend_manager.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/server/api_route.h"
#include "lemon/server/shared_args.h"
#include "lemon/system_info.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kResolveFailed{
    500, "",
    "Resolving the install parameters fails, for example for an unknown recipe/backend pair, an unsupported platform "
    "or a failed version lookup; the body also carries `arch` when the request set one"};

const RouteArg kRecipe{"recipe", ArgIn::JsonBody, {{"type", "string"}, {"minLength", 1}}, true, Support::Available,
                       "The recipe, such as `llamacpp`, `whispercpp` or `vllm`."};
const RouteArg kArch{"arch", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
                     "A ROCm GPU architecture, such as `gfx1201`, to resolve for in place of this host's "
                     "detected one, so CI can check an architecture's asset on a runner without that GPU."};

class InstallDryRunRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.install_dry_run";
        s.methods = {"POST"};
        s.paths = {"install/dry-run"};
        s.summary = "Resolve backend install metadata without downloading the backend asset";
        s.experimental = true;
        s.description =
            "Resolves the release asset that [`POST /v1/install`](#post-v1install) would download "
            "for a recipe/backend pair, without downloading or installing anything. Resolution "
            "reads local configuration and, for a backend version set to `latest`, GitHub release "
            "metadata.";
        s.notes = {
            "Errors carry an `error` string.",
            "A device ISA may resolve to a family target name used by the release repository, for "
            "example `gfx1201` to `gfx120X`. That mapping is defined by `rocm_asset_families` in "
            "`backend_versions.json`.",
            "Lemonade does not check that the asset exists; fetch `url`, or its split-archive "
            "manifest, to verify it.",
        };
        s.args = {kRecipe, backend_routes::kBackend, kArch};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["recipe", "backend", "arch", "repo", "version", "filename", "url",
                         "supports_split_archive", "supported"],
            "properties": {
                "recipe": {"type": "string", "description": "The requested recipe."},
                "backend": {"type": "string", "description": "The requested backend."},
                "arch": {"type": "string", "description": "The requested `arch`, or `\"\"` when it was omitted."},
                "repo": {"type": "string", "description": "GitHub repository of the release."},
                "version": {"type": "string", "description": "Release version: the pin in `backend_versions.json` unless runtime version policy overrides it."},
                "filename": {"type": "string", "description": "Release asset name."},
                "url": {"type": "string", "description": "Download URL built from `repo`, `version` and `filename`."},
                "supported": {"type": "boolean", "description": "Whether Lemonade's recipe/backend support matrix accepts the architecture. An unsupported `arch` still resolves, with `false`; without `arch`, a successful answer is always `true`."},
                "supports_split_archive": {"type": "boolean", "description": "Whether the recipe supports assets published as multiple archive parts. When `true`, the real download path can consult a `.partcount` manifest."}
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = ordered_json::parse(R"({
            "recipe": "whispercpp",
            "backend": "rocm",
            "arch": "gfx1201"
        })");
        s.examples = {example};
        s.errors = {kBodyNotJson, kArgMissing, kArgInvalid, kResolveFailed};
        s.request_format = RequestFormat::Json;
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        std::string requested_arch;
        try {
            const std::string recipe = req.arg(kRecipe).get<std::string>();
            const std::string backend = req.arg(backend_routes::kBackend).get<std::string>();
            requested_arch = req.value(kArch, "");

            if (!requested_arch.empty()) {
                SystemInfo::set_rocm_arch_override(requested_arch);
            }

            auto params = ctx_.backend_manager->get_install_params(recipe, backend);

            // Clear the override as soon as resolution is done so it cannot leak to
            // any later work on this thread.
            SystemInfo::set_rocm_arch_override("");

            const std::string url = "https://github.com/" + params.repo +
                                    "/releases/download/" + params.version + "/" +
                                    params.filename;

            bool supports_split_archive = false;
            if (auto* spec = backends::try_get_spec_for_recipe(recipe)) {
                supports_split_archive = spec->supports_split_archive;
            }

            bool supported = requested_arch.empty()
                ? true
                : SystemInfo::backend_supports_arch(recipe, backend, requested_arch);

            json response = {
                {"recipe", recipe},
                {"backend", backend},
                {"arch", requested_arch},
                {"repo", params.repo},
                {"version", params.version},
                {"filename", params.filename},
                {"url", url},
                {"supports_split_archive", supports_split_archive},
                {"supported", supported}
            };
            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& e) {
            SystemInfo::set_rocm_arch_override("");
            res.status = kResolveFailed.status;
            json error = {{"error", e.what()}};
            if (!requested_arch.empty()) {
                error["arch"] = requested_arch;
            }
            res.set_content(error.dump(), "application/json");
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_install_dry_run_route(ServerContext& ctx) {
    return std::make_unique<InstallDryRunRoute>(ctx);
}

} // namespace lemon

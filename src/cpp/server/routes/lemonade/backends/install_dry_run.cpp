#include <string>

#include "lemon/backend_manager.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/server/api_route.h"
#include "lemon/system_info.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

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
            "Resolve the backend install metadata that [`POST /v1/install`](#post-v1install)\n"
            "would use for a recipe/backend pair, without downloading or installing the\n"
            "backend asset. Nothing is installed and no existing installation is modified.\n\n"
            "Resolution uses the normal backend install-parameter machinery. It may consult\n"
            "local configuration and, if a backend version is configured as `latest`, query\n"
            "GitHub release metadata. The endpoint does not download the backend asset and\n"
            "does not check whether the returned asset URL exists.\n\n"
            "The `arch` parameter mocks ROCm GPU architecture detection while the install\n"
            "parameters are resolved. This makes the endpoint useful in CI for checking\n"
            "architecture-to-asset resolution on hardware that is not present on the runner.\n"
            "The repository's `test/server_gfx_topology.py` uses the endpoint for this\n"
            "resolution step and separately checks the resulting release URLs or\n"
            "split-archive manifests.";
        s.notes = {
            "A device ISA may resolve to a family target name used by the release repository, for "
            "example `gfx1201` to `gfx120X`. That mapping is defined by `rocm_asset_families` in "
            "`backend_versions.json`.",
            "An explicit architecture outside Lemonade's support matrix still produces install "
            "metadata, with `supported` set to `false`. To verify the release asset itself, check "
            "the returned URL, or its split-archive manifest, separately.",
            "A `400` answers a missing or empty `recipe` or `backend`. Invalid JSON, or "
            "install-parameter resolution failing (an unknown recipe/backend pair, an unsupported "
            "platform, unavailable architecture detection, failed version resolution), answers "
            "`500`. Errors carry an `error` string, plus `arch` when it was parsed before the "
            "failure.",
        };
        s.args = {
            {"recipe", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Recipe name, for example `llamacpp`, `whispercpp`, or `vllm`."},
            {"backend", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Backend name within the recipe, for example `vulkan`, `rocm`, or `rocm-nightly`."},
            {"arch", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "ROCm GPU architecture to use for this call, for example `gfx1201`. When provided, it "
             "overrides ROCm architecture detection while install parameters are resolved. When "
             "omitted, normal host detection is used; if resolution succeeds, `arch` is returned "
             "as `\"\"` and `supported` is `true`."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["recipe", "backend", "arch", "repo", "version", "filename", "url",
                         "supports_split_archive", "supported"],
            "properties": {
                "recipe": {"type": "string", "description": "Echo the requested values. If `arch` was omitted, it is returned as an empty string."},
                "backend": {"type": "string", "description": "Echo the requested values. If `arch` was omitted, it is returned as an empty string."},
                "arch": {"type": "string", "description": "Echo the requested values. If `arch` was omitted, it is returned as an empty string."},
                "repo": {"type": "string", "description": "Install parameters produced by the backend-specific resolver. The default version pin comes from `backend_versions.json`; runtime version policy can override it."},
                "version": {"type": "string", "description": "Install parameters produced by the backend-specific resolver. The default version pin comes from `backend_versions.json`; runtime version policy can override it."},
                "filename": {"type": "string", "description": "Install parameters produced by the backend-specific resolver. The default version pin comes from `backend_versions.json`; runtime version policy can override it."},
                "url": {"type": "string", "description": "GitHub release-download URL constructed from `repo`, `version`, and `filename`. The endpoint does not check this URL."},
                "supported": {"type": "boolean", "description": "Whether Lemonade's local recipe/backend support matrix accepts the requested `arch`. This is not a release-asset existence check. When `arch` is omitted, it is `true` on a successful response."},
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
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        std::string requested_arch;
        try {
            auto request_json = json::parse(req.http.body);

            const std::string recipe = request_json.value("recipe", "");
            const std::string backend = request_json.value("backend", "");
            requested_arch = request_json.value("arch", "");

            if (recipe.empty() || backend.empty()) {
                write_plain_error(res, 400, "Both 'recipe' and 'backend' are required");
                return;
            }

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
            res.status = 500;
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

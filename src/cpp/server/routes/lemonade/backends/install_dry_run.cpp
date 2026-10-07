#include <string>

#include "lemon/backend_manager.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/server/api_route.h"
#include "lemon/system_info.h"

namespace lemon {
namespace {

using json = nlohmann::json;

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
            "Resolves the release asset [`POST /v1/install`](#post-v1install) would download for "
            "a recipe and backend, optionally for a GPU architecture this machine lacks, without "
            "downloading or installing anything.";
        s.notes = {
            "Resolution uses the normal install-parameter machinery, so it may read local "
            "configuration and, for a backend pinned to `latest`, query GitHub release metadata. "
            "It neither downloads the asset nor checks that its URL exists. CI uses it to check "
            "architecture-to-asset resolution for hardware the runner lacks; "
            "`test/server_gfx_topology.py` checks the resulting URLs separately.",
            "An architecture can resolve to a family target name the release repository uses: "
            "`gfx1201` resolves to `gfx120X`, as defined by `rocm_asset_families` in "
            "`backend_versions.json`. An `arch` outside Lemonade's support matrix still produces "
            "metadata, with `supported: false`.",
            "Missing `recipe` or `backend` is answered with `400`. Invalid JSON, or a failure to "
            "resolve (an unknown recipe or backend, an unsupported platform, unavailable "
            "architecture detection, or a failed version lookup), is answered with `500` and an "
            "`error` string, plus `arch` when it was parsed.",
        };
        s.args = {
            {"recipe", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Recipe name, e.g. `llamacpp`, `whispercpp` or `vllm`."},
            {"backend", ArgIn::JsonBody, {{"type", "string"}}, true, Support::Available,
             "Backend within the recipe, e.g. `vulkan`, `rocm` or `rocm-nightly`."},
            {"arch", ArgIn::JsonBody, {{"type", "string"}}, false, Support::Available,
             "ROCm GPU architecture to resolve for, e.g. `gfx1201`, overriding detection for this "
             "call. Omitted, the host is detected."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = json::parse(R"({
            "type": "object",
            "required": ["recipe", "backend", "arch", "repo", "version", "filename", "url",
                         "supports_split_archive", "supported"],
            "properties": {
                "recipe": {"type": "string"},
                "backend": {"type": "string"},
                "arch": {"type": "string", "description": "The requested arch, or \"\" when omitted."},
                "repo": {"type": "string", "description": "GitHub repository of the release."},
                "version": {"type": "string", "description": "Release version: the pin in backend_versions.json unless a runtime version policy overrides it."},
                "filename": {"type": "string", "description": "Release asset name."},
                "url": {"type": "string", "description": "Release download URL built from repo, version and filename. Not checked."},
                "supports_split_archive": {"type": "boolean", "description": "Whether the recipe's assets may be published in parts, which the real download finds through a .partcount manifest."},
                "supported": {"type": "boolean", "description": "Whether Lemonade's support matrix accepts arch for this recipe and backend. Always true when arch is omitted. Not an asset existence check."}
            }
        })");
        response.example = json::parse(R"({
            "recipe": "whispercpp",
            "backend": "rocm",
            "arch": "gfx1201"
        })");
        s.responses = {response};
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

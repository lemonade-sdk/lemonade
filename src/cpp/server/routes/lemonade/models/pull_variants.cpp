#include <lemon/utils/aixlog.hpp>

#include "lemon/hf_variants.h"
#include "lemon/model_registry.h"
#include "lemon/runtime_config.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

class PullVariantsRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.pull_variants";
        s.methods = {"GET"};
        s.paths = {"pull/variants"};
        s.summary = "Enumerate GGUF variants for a Hugging Face checkpoint";
        s.description =
            "Inspect a Hugging Face GGUF repository and enumerate the variants (quantizations and "
            "sharded folder groups) available for installation. Used by the `lemonade pull "
            "<owner/repo>` CLI flow and by the desktop app's model search to auto-populate the "
            "install form. The endpoint reads only public Hugging Face metadata; if the `HF_TOKEN` "
            "environment variable is set on the server, it is forwarded as a bearer token to "
            "access gated repositories.";
        s.args = {
            {"checkpoint", ArgIn::Query, {{"type", "string"}}, true, Support::Available,
             "Hugging Face repo id, e.g. `unsloth/Qwen3-8B-GGUF`. Passed as a query string. A "
             "Hugging Face or ModelScope URL is normalized to `owner/repo` and selects its "
             "registry."},
            {"source", ArgIn::Query, {{"type", "string"}}, false, Support::Available,
             "Registry: `huggingface` or `modelscope`. Defaults to the server's "
             "`default_model_source`."},
        };

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["checkpoint", "source", "recipe", "repo_kind", "suggested_name", "suggested_labels", "mmproj_files", "draft_files", "variants"],
            "properties": {
                "checkpoint": {"type": "string", "description": "Echoed input (normalized to `owner/repo` when given as a URL)."},
                "source": {"type": "string", "description": "Registry the variants came from."},
                "recipe": {"type": "string", "description": "Suggested recipe (`llamacpp` for a GGUF repo, `ryzenai-llm` for an ONNX RyzenAI repo, `collection.omni` for an exported collection; future expansion may return other values)."},
                "repo_kind": {"enum": ["gguf", "onnx-ryzenai", "collection"]},
                "suggested_name": {"type": "string", "description": "Repo id stripped of the `owner/` prefix; suitable for use as the `user.<name>` model name."},
                "suggested_labels": {"type": "array", "items": {"type": "string"},
                                     "description": "Inferred labels — `vision` if any `mmproj-*.gguf` files exist, `mtp` or `dflash` if a variant ships a draft model, plus `embeddings`/`reranking` if those substrings appear in the repo id, and the recipe's deployment label (e.g. `chat`) when no other label names one."},
                "mmproj_files": {"type": "array", "items": {"type": "string"},
                                 "description": "Bare filenames of `mmproj-*.gguf` files in the repo; the first one should be passed as `mmproj` to `/v1/pull` for vision models."},
                "draft_files": {"type": "array", "items": {"type": "string"}, "description": "Bare names of draft-model GGUF files."},
                "variants": {"type": "array", "description": "Quantizations for the repo. Each entry has `name` (e.g. `Q4_K_M`, `UD-Q4_K_XL`), `primary_file`, `files`, `sharded`, and `size_bytes` (from the HF `?blobs=true` listing). Ranked by frequency of use in `server_models.json` (`Q4_K_M`, `UD-Q4_K_XL`, `Q8_0`, `Q4_0` first, everything else sorted lexicographically). The CLI `lemonade pull` menu also accepts a free-text variant name.", "items": {
                    "type": "object",
                    "required": ["name", "primary_file", "files", "sharded", "size_bytes"],
                    "properties": {
                        "name": {"type": "string"},
                        "primary_file": {"type": "string"},
                        "draft_file": {"type": "string", "description": "The draft model paired with this variant, when the repository ships one."},
                        "files": {"type": "array", "items": {"type": "string"}},
                        "sharded": {"type": "boolean"},
                        "size_bytes": {"type": "integer"}
                    }
                }},
                "size": {"type": "number", "description": "collection only: the collection's total size, from its manifest."},
                "component_count": {"type": "integer", "description": "collection only: how many models the collection bundles."}
            }
        })");
        response.example = {{"checkpoint", "unsloth/Qwen3-8B-GGUF"}};
        s.responses = {response};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        const httplib::Request& http = req.http;
        try {
            std::string checkpoint = http.get_param_value("checkpoint");
            if (checkpoint.empty()) {
                write_plain_error(res, 400, "Missing required query parameter 'checkpoint'");
                return;
            }
            // Detect the URL provider upfront so we can reject mismatches with
            // an explicit `--source` param (just like /pull and the CLI do).
            lemon::RemoteRegistrySource url_source;
            std::string url_repo_id;
            bool has_url = lemon::detect_registry_url(checkpoint, url_source, url_repo_id);

            std::string raw_source;
            if (http.has_param("source")) {
                raw_source = http.get_param_value("source");
            }

            // Canonicalize the raw param first so accepted aliases (e.g. `hf`) are
            // not rejected against the URL's canonical registry name.
            if (has_url && !raw_source.empty()) {
                if (parse_remote_registry_source(raw_source) != url_source) {
                    write_plain_error(res, 400,
                        "checkpoint URL uses " + remote_registry_source_name(url_source) +
                        " but source was set to '" + raw_source + "'");
                    return;
                }
            }

            // A provider URL is authoritative: normalize it to owner/repo and adopt
            // its registry. Otherwise honor an explicit source param, falling back
            // to the server's configured default when none is given.
            if (has_url) {
                checkpoint = url_repo_id;
            }
            std::string source;
            if (has_url) {
                source = remote_registry_source_name(url_source);
            } else {
                source = http.has_param("source")
                    ? http.get_param_value("source") : ctx_.config->default_model_source();
            }
            if (checkpoint.find('/') == std::string::npos) {
                write_plain_error(res, 400,
                    "Malformed 'checkpoint': expected a repository id of the form 'owner/name'");
                return;
            }
            const auto parsed_source = parse_remote_registry_source(source);
            if (ctx_.config->offline()) {
                res.status = 400;
                json error = {{"error", "Lemond is in offline mode, models not downloaded"}, {"code", "lemond_offline"}};
                res.set_content(error.dump(), "application/json");
                return;
            }
            bool not_found = false;
            const std::string resolved_source = remote_registry_source_name(parsed_source);
            json body = lemon::fetch_pull_variants(checkpoint, resolved_source, not_found);
            if (not_found) {
                write_plain_error(res, 404, "Checkpoint '" + checkpoint + "' not found on " +
                    remote_registry_display_name(parsed_source));
                return;
            }
            if (body.is_object()) {
                body["source"] = resolved_source;
            }
            res.set_content(body.dump(), "application/json");
        } catch (const std::invalid_argument& e) {
            write_plain_error(res, 400, e.what());
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in handle_pull_variants: " << e.what() << std::endl;
            write_plain_error(res, 500, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_pull_variants_route(ServerContext& ctx) {
    return std::make_unique<PullVariantsRoute>(ctx);
}

} // namespace lemon

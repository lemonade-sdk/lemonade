#include <lemon/utils/aixlog.hpp>

#include "lemon/server/model_sync.h"
#include "lemon/hf_variants.h"
#include "lemon/model_registry.h"
#include "lemon/runtime_config.h"
#include "lemon/server/api_route.h"

namespace lemon {
namespace {

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;

const RouteError kMissingCheckpoint{400, "", "`checkpoint` is missing or empty"};
const RouteError kSourceMismatch{
    400, "", "`checkpoint` is a URL whose registry disagrees with `source`"};
const RouteError kMalformedCheckpoint{400, "", "`checkpoint` does not contain `/`"};
const RouteError kUnsupportedRepo{
    400, "",
    "`source` names an unsupported registry, or the Hugging Face GGUF repository is one "
    "llama.cpp cannot run"};
const RouteError kCheckpointNotFound{404, "", "The registry has no such checkpoint"};
const RouteError kVariantsFailed{
    500, "",
    "Another failure occurs, for example a repository with no GGUF, ONNX RyzenAI or collection "
    "files, or a network error"};

const RouteArg kCheckpoint{"checkpoint", ArgIn::Query, {{"type", "string"}}, true, Support::Available,
                           "Repository id, e.g. `unsloth/Qwen3-8B-GGUF`. A Hugging Face or ModelScope URL is "
                           "normalized to `owner/repo` and selects its registry."};
const RouteArg kSource{"source", ArgIn::Query, {{"type", "string"}}, false, Support::Available,
                       "Registry: `huggingface` or `modelscope`. Defaults to the server's "
                       "`default_model_source`."};

class PullVariantsRoute : public ApiRoute {
public:
    using ApiRoute::ApiRoute;

    RouteSpec spec() const override {
        RouteSpec s;
        s.id = "lemonade.pull_variants";
        s.methods = {"GET"};
        s.paths = {"pull/variants"};
        s.summary = "List the installable variants of a model repository";
        s.description =
            "Inspects a Hugging Face or ModelScope repository and lists the variants it offers "
            "for installation, such as quantizations and sharded folder groups. The `lemonade pull "
            "<owner/repo>` command and the desktop app's model search use it to fill in the "
            "install form. Lemonade reads public registry metadata, and sends the server's "
            "`HF_TOKEN` environment variable, when set, to Hugging Face for gated repositories.";
        s.args = {kCheckpoint, kSource};

        RouteResponse response;
        response.format = ResponseFormat::Json;
        response.schema = ordered_json::parse(R"({
            "type": "object",
            "required": ["checkpoint", "source", "recipe", "repo_kind", "suggested_name", "suggested_labels", "mmproj_files", "draft_files", "variants"],
            "properties": {
                "checkpoint": {"type": "string", "description": "Echoed input (normalized to `owner/repo` when given as a URL)."},
                "source": {"type": "string", "description": "Registry the variants came from."},
                "recipe": {"type": "string", "description": "Suggested recipe: `llamacpp` for a GGUF repo, `ryzenai-llm` for an ONNX RyzenAI repo, `collection.omni` for an exported collection."},
                "repo_kind": {"enum": ["gguf", "onnx-ryzenai", "collection"], "description": "Kind of repository: `gguf`, `onnx-ryzenai` or `collection`."},
                "suggested_name": {"type": "string", "description": "Repo id stripped of the `owner/` prefix; suitable for use as the `user.<name>` model name."},
                "suggested_labels": {"type": "array", "items": {"type": "string"},
                                     "description": "Inferred labels: `vision` if any GGUF file name contains `mmproj`, `mtp` or `dflash` if a variant ships a draft model, plus `embeddings` or `reranking` if the repo id contains `embed` or `rerank`, and the recipe's deployment label (e.g. `chat`) when no other label names one."},
                "mmproj_files": {"type": "array", "items": {"type": "string"},
                                 "description": "Bare filenames of the repo's GGUF files whose name contains `mmproj`; pass the first as `mmproj` to [`POST /v1/pull`](#post-v1pull) for a vision model."},
                "draft_files": {"type": "array", "items": {"type": "string"}, "description": "Bare names of draft-model GGUF files."},
                "variants": {"type": "array", "description": "Installable variants, ranked by how often `server_models.json` uses each quantization: `Q4_K_M`, `UD-Q4_K_XL`, `Q8_0` and `Q4_0` first, then the rest in lexicographic order.", "items": {
                    "type": "object",
                    "required": ["name", "primary_file", "files", "sharded", "size_bytes"],
                    "properties": {
                        "name": {"type": "string", "description": "Variant name, such as `Q4_K_M`."},
                        "primary_file": {"type": "string", "description": "The file to load."},
                        "draft_file": {"type": "string", "description": "The draft model paired with this variant, when the repository ships one."},
                        "files": {"type": "array", "items": {"type": "string"}, "description": "Every file in the variant."},
                        "sharded": {"type": "boolean", "description": "Whether the variant is split across files."},
                        "size_bytes": {"type": "integer", "description": "Total size, from the registry's file listing."}
                    }
                }},
                "size": {"type": "number", "description": "Collections only: the collection's total size, from its manifest."},
                "component_count": {"type": "integer", "description": "Collections only: how many models the collection bundles."}
            }
        })");
        s.responses = {response};

        RouteExample example;
        example.format = ResponseFormat::Json;
        example.args = {{"checkpoint", "unsloth/Qwen3-8B-GGUF"}};
        s.examples = {example};
        s.errors = {kMissingCheckpoint, kSourceMismatch, kMalformedCheckpoint,
                    kUnsupportedRepo, kLemondOffline, kCheckpointNotFound, kVariantsFailed};
        return s;
    }

    void handle(RouteRequest& req, httplib::Response& res) override {
        try {
            std::string checkpoint = req.value(kCheckpoint, "");
            if (checkpoint.empty()) {
                write_plain_error(res, kMissingCheckpoint, "Missing required query parameter 'checkpoint'");
                return;
            }
            lemon::RemoteRegistrySource url_source;
            std::string url_repo_id;
            bool has_url = lemon::detect_registry_url(checkpoint, url_source, url_repo_id);

            const std::string raw_source = req.value(kSource, "");

            // Canonicalize the raw param first so accepted aliases (e.g. `hf`) are
            // not rejected against the URL's canonical registry name.
            if (has_url && !raw_source.empty()) {
                if (parse_remote_registry_source(raw_source) != url_source) {
                    write_plain_error(res, kSourceMismatch,
                        "checkpoint URL uses " + remote_registry_source_name(url_source) +
                        " but source was set to '" + raw_source + "'");
                    return;
                }
            }

            if (has_url) {
                checkpoint = url_repo_id;
            }
            std::string source;
            if (has_url) {
                source = remote_registry_source_name(url_source);
            } else {
                source = req.has(kSource)
                    ? req.arg(kSource).get<std::string>() : ctx_.config->default_model_source();
            }
            if (checkpoint.find('/') == std::string::npos) {
                write_plain_error(res, kMalformedCheckpoint,
                    "Malformed 'checkpoint': expected a repository id of the form 'owner/name'");
                return;
            }
            const auto parsed_source = parse_remote_registry_source(source);
            if (refuse_offline_download(ctx_, res)) {
                return;
            }
            bool not_found = false;
            const std::string resolved_source = remote_registry_source_name(parsed_source);
            json body = lemon::fetch_pull_variants(checkpoint, resolved_source, not_found);
            if (not_found) {
                write_plain_error(res, kCheckpointNotFound, "Checkpoint '" + checkpoint + "' not found on " +
                    remote_registry_display_name(parsed_source));
                return;
            }
            if (body.is_object()) {
                body["source"] = resolved_source;
            }
            res.set_content(body.dump(), "application/json");
        } catch (const std::invalid_argument& e) {
            write_plain_error(res, kUnsupportedRepo, e.what());
        } catch (const std::exception& e) {
            LOG(ERROR, "Server") << "ERROR in handle_pull_variants: " << e.what() << std::endl;
            write_plain_error(res, kVariantsFailed, e.what());
        }
    }
};

} // namespace

std::unique_ptr<ApiRoute> make_pull_variants_route(ServerContext& ctx) {
    return std::make_unique<PullVariantsRoute>(ctx);
}

} // namespace lemon

#include "lemon/server/job_ops.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>

#include <lemon/utils/aixlog.hpp>

#include "lemon/error_types.h"
#include "lemon/jobs/job_media_io.h"
#include "lemon/jobs/job_media_limits.h"
#include "lemon/model_manager.h"
#include "lemon/model_types.h"
#include "lemon/router.h"
#include "lemon/server.h"
#include "lemon/server/api_route.h"
#include "lemon/server/audio_formats.h"
#include "lemon/server/model_json.h"
#include "lemon/server/model_loader.h"
#include "lemon/server/system_stats.h"
#include "lemon/server/video_store.h"
#include "lemon/system_info.h"
#include "lemon/system_metrics_platform.h"
#include "lemon/utils/json_utils.h"
#include "lemon/wrapped_server.h"

namespace lemon {

namespace {

std::string error_message(const nlohmann::json& response, const std::string& fallback) {
    const auto err = response.find("error");
    if (err == response.end()) return fallback;
    if (err->is_object()) {
        const auto message = err->find("message");
        if (message != err->end() && message->is_string()) return message->get<std::string>();
    }
    if (err->is_string()) return err->get<std::string>();
    return fallback;
}

lemon::jobs::json image_op_result(const nlohmann::json& response, const std::string& op) {
    if (response.contains("error"))
        throw lemon::jobs::JobError(424, error_message(response, op + " failed"));
    size_t bytes = 0;
    if (response.contains("data") && response["data"].is_array()) {
        for (const auto& item : response["data"]) {
            if (item.is_object() && item.contains("b64_json") && item["b64_json"].is_string())
                bytes += lemon::jobs::base64_decoded_size(
                    item["b64_json"].get_ref<const std::string&>());
        }
    }
    if (!lemon::jobs::media_output_within_limit(bytes))
        throw lemon::jobs::JobError(413, op + " output exceeds the job media output limit of " +
                                       std::to_string(lemon::jobs::kMaxJobMediaOutputBytes) + " bytes");
    lemon::jobs::json out = response;
    if (bytes > 0) out["size"] = bytes;
    return out;
}

void throw_if_format_error(const AudioFormat& format) {
    if (format.error.empty()) return;
    if (format.error.find("'" + format.format + "'") != std::string::npos)
        throw lemon::jobs::JobError(400, format.error);
    throw lemon::jobs::JobError(400, format.error + ": '" + format.format + "'");
}

void throw_if_cancelled(const lemon::jobs::CancelFlag& cancel) {
    if (cancel.load()) throw lemon::jobs::JobError(499, "interrupted");
}

lemon::jobs::json media_op_result(const std::string& op, lemon::jobs::CancelFlag& cancel,
                                  const std::function<void(httplib::DataSink&)>& generate,
                                  const std::string& mime_type,
                                  const std::string& response_format,
                                  const std::map<std::string, std::string>& headers = {}) {
    lemon::jobs::MediaSinkCapture capture(cancel);
    httplib::DataSink sink;
    sink.write = [&](const char* data, size_t len) { return capture.write(data, len); };
    sink.is_writable = [&]() { return capture.writable(); };
    sink.done = []() {};
    {
        WrappedServer::RequestCancelScope cancel_scope(utils::RequestCancelToken{&cancel});
        generate(sink);
    }
    throw_if_cancelled(cancel);
    if (!capture.stream_error().is_null())
        throw lemon::jobs::JobError(424, error_message(capture.stream_error(), op + " failed"));
    if (capture.overflowed()) {
        throw lemon::jobs::JobError(413, op + " output exceeds the job media output limit of " +
                                             std::to_string(lemon::jobs::kMaxJobMediaOutputBytes) + " bytes");
    }
    if (capture.buffer().empty()) throw lemon::jobs::JobError(502, op + " produced no output");
    if (auto error_payload = extract_error_payload(capture.buffer()); !error_payload.is_null())
        throw lemon::jobs::JobError(424, error_message(error_payload, op + " failed"));
    return lemon::jobs::media_result_json(
        lemon::utils::JsonUtils::base64_encode(capture.buffer()), capture.buffer().size(),
        mime_type, response_format, headers);
}

} // namespace

lemon::jobs::OpProviders JobOps::build(ServerContext& ctx) {
    lemon::jobs::OpProviders providers;
    Router* router = ctx.router;
    ModelManager* model_manager = ctx.model_manager;
    ModelJson* model_json = ctx.model_json;
    SystemMetricsPlatform* metrics = ctx.metrics;
    Server* server = ctx.server;
    ModelLoader* model_loader = ctx.model_loader;

    // A running job remembers which models were loaded before it started (snapshot)
    // and which ones it loaded itself (owned), so pausing it can unload exactly its own
    // models and resuming can restore them.
    struct JobModelState {
        std::map<std::string, bool> snapshot;
        std::map<std::string, nlohmann::json> owned;
    };
    auto job_states = std::make_shared<std::map<std::string, JobModelState>>();
    auto current_job = std::make_shared<std::string>();
    auto state_mutex = std::make_shared<std::mutex>();

    providers.system_info = [] {
        return lemon::jobs::json::parse(SystemInfoCache::get_system_info_with_cache().dump());
    };
    providers.system_stats = [server, metrics] {
        return lemon::jobs::json::parse(system_stats_json(*server, *metrics).dump());
    };
    providers.models_list = [model_manager, model_json] {
        nlohmann::json data = nlohmann::json::array();
        for (const auto& [model_id, info] : model_manager->get_supported_models())
            data.push_back(model_json->to_json(model_id, info));
        nlohmann::json response = {{"object", "list"}, {"data", data}};
        return lemon::jobs::json::parse(response.dump());
    };
    providers.model_get = [model_manager, model_json](const std::string& id) -> lemon::jobs::json {
        auto resolved = model_manager->resolve(id);
        if (!resolved) return lemon::jobs::json(nullptr);
        return lemon::jobs::json::parse(
            model_json->to_json(resolved->wire_id, *resolved->info).dump());
    };
    providers.load_op = [router, model_manager, job_states, current_job, state_mutex](
                            const lemon::jobs::json& params,
                            lemon::jobs::CancelFlag& cancel) -> lemon::jobs::json {
        if (!params.contains("model") || !params["model"].is_string())
            throw lemon::jobs::JobError(400, "load requires a 'model' string");
        const std::string model = params["model"].get<std::string>();
        if (!model_manager->model_exists(model))
            throw lemon::jobs::JobError(404, "unknown model '" + model + "'");
        if (!model_manager->is_model_downloaded(model))
            throw lemon::jobs::JobError(404, "model '" + model + "' is not downloaded");
        auto info = model_manager->get_model_info(model);
        nlohmann::json opt_json = params;
        RecipeOptions options(info.recipe, opt_json);
        std::optional<bool> pinned = std::nullopt;
        if (params.contains("pinned") && params["pinned"].is_boolean())
            pinned = params["pinned"].get<bool>();
        try {
            router->load_model(model, info, options, true, true, pinned,
                               LoadPurpose::UserInference, &cancel);
        } catch (const ModelLoadRefusedException& e) {
            throw lemon::jobs::JobError(400, e.what());
        } catch (const std::exception& e) {
            throw lemon::jobs::JobError(500, e.what());
        }
        {
            const std::string canonical = router->canonical_model_name(model);
            const int pid = router->loaded_model_pid(model);
            std::lock_guard<std::mutex> lk(*state_mutex);
            if (!current_job->empty()) {
                auto it = job_states->find(*current_job);
                if (it != job_states->end())
                    it->second.owned[canonical] = {{"pid", pid}};
            }
        }
        if (cancel.load()) {
            throw lemon::jobs::JobError(499, "interrupted");
        }
        RecipeOptions effective = router->get_model_recipe_options(model);
        lemon::jobs::json out;
        out["loaded"] = true;
        out["model"] = model;
        const nlohmann::json backend_json = effective.get_option(info.recipe + "_backend");
        if (backend_json.is_string()) out["backend"] = backend_json.get<std::string>();
        const nlohmann::json ctx_json = effective.get_option("ctx_size");
        if (ctx_json.is_number()) out["ctx_size"] = ctx_json.get<int64_t>();
        return out;
    };
    providers.unload_op = [router, job_states, current_job, state_mutex](
                              const lemon::jobs::json& params,
                              lemon::jobs::CancelFlag&) -> lemon::jobs::json {
        std::string model;
        if (params.contains("model") && params["model"].is_string())
            model = params["model"].get<std::string>();
        if (!model.empty()) {
            if (router->is_model_loaded(model)) router->unload_model(model);
        } else {
            router->unload_model("");
        }
        {
            std::lock_guard<std::mutex> lk(*state_mutex);
            if (!current_job->empty()) {
                auto it = job_states->find(*current_job);
                if (it != job_states->end()) {
                    if (model.empty()) it->second.owned.clear();
                    else it->second.owned.erase(router->canonical_model_name(model));
                }
            }
        }
        return lemon::jobs::json::object();
    };
    providers.chat_op = [router](const lemon::jobs::json& params,
                                 lemon::jobs::CancelFlag& cancel) -> lemon::jobs::json {
        nlohmann::json request = params;
        nlohmann::json response = router->chat_completion(request, &cancel);
        if (response.contains("error"))
            throw lemon::jobs::JobError(424, error_message(response, "chat failed"));
        return lemon::jobs::json::parse(response.dump());
    };
    // generate_video does not take the router's exclusive slot, so a clip does not
    // hold up other models; the slot is also what records job-owned models, so
    // the op unloads a model it loaded itself when interrupted, unless the
    // backend confirms its job settled cooperatively and can be reused.
    providers.video_op = [router, model_loader, model_manager](const lemon::jobs::json& params,
                                                               const std::string& job_id,
                                                               lemon::jobs::CancelFlag& cancel)
        -> lemon::jobs::json {
        nlohmann::json request = nlohmann::json::parse(params.dump());
        if (const std::string error = video_store::normalize_request(request); !error.empty())
            throw lemon::jobs::JobError(400, error);
        const std::string model = request.value("model", std::string());
        // clip_file comes from the job's params, which a generic job can leave out or fill
        // with another video's name, so the op always writes to a name it reserved itself.
        const std::string file =
            video_store::reserve_file_name(request.value("clip_file", std::string()), job_id);
        bool published = false;
        struct ReleaseUnpublished {
            const std::string& file;
            const bool& published;
            ~ReleaseUnpublished() {
                if (!published) video_store::release(file);
            }
        } release_unpublished{file, published};
        std::map<std::string, int> loaded_here;
        auto unload_if_interrupted = [&] {
            if (cancel.load() && !loaded_here.empty())
                router->unload_job_models(loaded_here, {});
        };
        if (!model.empty()) {
            // Checked before loading: naming a chat model would otherwise download it and
            // evict a loaded model before the router turns it away. An unknown model is
            // left to the load, which reports it.
            std::optional<ModelInfo> info;
            try {
                info = model_manager->get_model_info(model);
            } catch (const std::exception&) {
            }
            if (info && get_model_type_from_labels(info->labels) != ModelType::VIDEO)
                throw lemon::jobs::JobError(400, "model '" + model + "' is not a video model");
            const bool was_loaded = router->is_model_loaded(model);
            model_loader->ensure_loaded(
                model, ModelLoader::load_options(request.value("ctx_size", nlohmann::json())),
                LoadPurpose::UserInference, &cancel);
            if (!was_loaded)
                loaded_here[router->canonical_model_name(model)] = router->loaded_model_pid(model);
            if (cancel.load()) {
                unload_if_interrupted();
                throw lemon::jobs::JobError(499, "interrupted");
            }
        }
        struct ClearProgress {
            const std::string& job_id;
            ~ClearProgress() { video_store::clear_progress(job_id); }
        } clear_progress{job_id};
        nlohmann::json response = router->video_generations(
            request, &cancel, [&job_id](const nlohmann::json& progress) {
                video_store::set_progress(job_id, progress);
            });
        if (response.contains("error")) {
            const auto& err = response["error"];
            const bool settled = err.is_object() && err.contains("details") &&
                err["details"].is_object() && err["details"].contains("backend_job_settled") &&
                err["details"]["backend_job_settled"] == true;
            if (!settled) unload_if_interrupted();
            if (cancel.load()) throw lemon::jobs::JobError(499, "interrupted");
            std::string msg = "video generation failed";
            if (err.is_object() && err.contains("message") && err["message"].is_string())
                msg = err["message"].get<std::string>();
            else if (err.is_string())
                msg = err.get<std::string>();
            throw lemon::jobs::JobError(424, msg);
        }
        // Completion may race with cancel. Do not publish a canceled clip.
        if (cancel.load()) throw lemon::jobs::JobError(499, "interrupted");
        if (response.contains("b64_json") && response["b64_json"].is_string()) {
            video_store::write(file, utils::JsonUtils::base64_decode(response["b64_json"].get<std::string>()));
            published = true;
            response.erase("b64_json");
            response["clip_file"] = file;
        }
        if (!response.contains("clip_file")) {
            throw lemon::jobs::JobError(424, "video generation returned no clip");
        }
        return lemon::jobs::json::parse(response.dump());
    };
    providers.image_generations_op = [router](
        const lemon::jobs::json& params, lemon::jobs::CancelFlag& cancel) -> lemon::jobs::json {
        throw_if_cancelled(cancel);
        nlohmann::json request = params;
        nlohmann::json response = router->image_generations(request, &cancel);
        throw_if_cancelled(cancel);
        auto out = image_op_result(response, "image_generations");
        return out;
    };
    providers.image_edits_op = [router](
        const lemon::jobs::json& params, lemon::jobs::CancelFlag& cancel) -> lemon::jobs::json {
        throw_if_cancelled(cancel);
        nlohmann::json request = params;
        lemon::jobs::normalize_image_inputs(request, true);
        nlohmann::json response = router->image_edits(request, &cancel);
        throw_if_cancelled(cancel);
        auto out = image_op_result(response, "image_edits");
        return out;
    };
    providers.image_variations_op = [router](
        const lemon::jobs::json& params, lemon::jobs::CancelFlag& cancel) -> lemon::jobs::json {
        throw_if_cancelled(cancel);
        nlohmann::json request = params;
        lemon::jobs::normalize_image_inputs(request, true);
        nlohmann::json response = router->image_variations(request, &cancel);
        throw_if_cancelled(cancel);
        auto out = image_op_result(response, "image_variations");
        return out;
    };
    providers.audio_speech_op = [router](
        const lemon::jobs::json& params, lemon::jobs::CancelFlag& cancel) -> lemon::jobs::json {
        throw_if_cancelled(cancel);
        nlohmann::json request = params;
        // Jobs buffer the result, so streaming selectors cannot be honoured.
        request.erase("stream");
        request.erase("stream_format");
        const std::string model = request.value("model", "");
        const AudioFormat format = negotiate_speech_format(
            *router, model, request.value("response_format", nlohmann::json()),
            false);
        throw_if_format_error(format);
        const auto info = router->try_get_model_info(model);
        if (!info) throw lemon::jobs::JobError(404, "unknown model '" + model + "'");
        if (info->type != ModelType::TTS)
            throw lemon::jobs::JobError(400, "model '" + model + "' is not a text-to-speech model");
        request["response_format"] = format.format;
        if (request.contains("voice") && request["voice"] == "") request.erase("voice");
        auto out = media_op_result(
            "audio_speech", cancel,
            [&](httplib::DataSink& sink) { router->audio_speech(request, sink); },
            format.mime_type, format.format, format.headers);
        return out;
    };
    providers.audio_generations_op = [router](
        const lemon::jobs::json& params, lemon::jobs::CancelFlag& cancel) -> lemon::jobs::json {
        throw_if_cancelled(cancel);
        nlohmann::json request = params;
        // Jobs buffer the result, so streaming selectors cannot be honoured.
        request.erase("stream");
        request.erase("stream_format");
        const std::string model = request.value("model", "");
        AudioFormat format = negotiate_audio_generation_format(
            *router, model, request.value("response_format", nlohmann::json()));
        // Jobs validate formats strictly; the HTTP endpoint falls back to a default format.
        if (format.error.empty() && router->audio_generation_supported_formats(model).empty() &&
            !is_known_audio_format(format.format))
            format.error = "Unsupported audio format requested";
        throw_if_format_error(format);
        const auto info = router->try_get_model_info(model);
        if (!info) throw lemon::jobs::JobError(404, "unknown model '" + model + "'");
        if (info->type != ModelType::AUDIO_GENERATION)
            throw lemon::jobs::JobError(400, "model '" + model + "' is not an audio-generation model");
        request["response_format"] = format.format;
        auto out = media_op_result(
            "audio_generations", cancel,
            [&](httplib::DataSink& sink) { router->audio_generations(request, sink); },
            format.mime_type, format.format, format.headers);
        return out;
    };
    providers.model_3d_generations_op = [router](
        const lemon::jobs::json& params, lemon::jobs::CancelFlag& cancel) -> lemon::jobs::json {
        throw_if_cancelled(cancel);
        nlohmann::json request = params;
        lemon::jobs::normalize_image_inputs(request, false);
        auto out = media_op_result(
            "model_3d_generations", cancel,
            [&](httplib::DataSink& sink) { router->model_3d_generations(request, sink); },
            "model/gltf-binary", "glb");
        return out;
    };
    providers.begin_exclusive = [router, job_states, current_job, state_mutex](
                                    const std::string& job_id,
                                    lemon::jobs::CancelFlag* cancel) -> bool {
        if (!router->begin_exclusive(cancel)) return false;
        std::lock_guard<std::mutex> lk(*state_mutex);
        if (!job_states->count(job_id))
            (*job_states)[job_id].snapshot = router->snapshot_loaded_models();
        *current_job = job_id;
        return true;
    };
    providers.end_exclusive = [router, current_job, state_mutex] {
        {
            std::lock_guard<std::mutex> lk(*state_mutex);
            current_job->clear();
        }
        router->end_exclusive();
    };
    providers.reconcile_unload = [router, job_states, state_mutex](
                                     const std::string& job_id) {
        std::map<std::string, int> owned_live;
        std::map<std::string, bool> snapshot;
        {
            std::lock_guard<std::mutex> lk(*state_mutex);
            auto it = job_states->find(job_id);
            if (it == job_states->end()) return;
            for (const auto& kv : it->second.owned)
                if (kv.second.is_object() && kv.second.contains("pid"))
                    owned_live[kv.first] = kv.second["pid"].get<int>();
            snapshot = it->second.snapshot;
        }
        if (owned_live.empty()) return;
        auto captured = router->unload_job_models(owned_live, snapshot);
        std::lock_guard<std::mutex> lk(*state_mutex);
        auto it = job_states->find(job_id);
        if (it == job_states->end()) return;
        for (auto& kv : captured) it->second.owned[kv.first] = std::move(kv.second);
    };
    providers.restore_exclusive = [router, model_manager, job_states, state_mutex](
                                      const std::string& job_id,
                                      const lemon::jobs::json& manifest,
                                      lemon::jobs::CancelFlag* cancel) -> bool {
        std::map<std::string, nlohmann::json> to_restore;
        std::set<std::string> tracked;
        {
            std::lock_guard<std::mutex> lk(*state_mutex);
            auto it = job_states->find(job_id);
            if (it != job_states->end()) {
                for (const auto& kv : it->second.owned) {
                    tracked.insert(kv.first);
                    if (kv.second.is_object() && kv.second.contains("options"))
                        to_restore[kv.first] = kv.second;
                }
            }
        }
        for (auto it = manifest.begin(); it != manifest.end(); ++it) {
            const std::string canonical = router->canonical_model_name(it.key());
            if (tracked.count(canonical) || to_restore.count(canonical)) continue;
            nlohmann::json params = nlohmann::json::parse(it.value().dump());
            nlohmann::json entry = {{"options", params}};
            if (params.contains("pinned") && params["pinned"].is_boolean())
                entry["pinned"] = params["pinned"].get<bool>();
            to_restore[canonical] = std::move(entry);
        }
        for (const auto& kv : to_restore) {
            if (cancel && cancel->load()) return false;
            if (router->is_model_loaded(kv.first)) {
                std::lock_guard<std::mutex> lk(*state_mutex);
                auto it = job_states->find(job_id);
                if (it != job_states->end()) it->second.owned.erase(kv.first);
                continue;
            }
            try {
                auto info = model_manager->get_model_info(kv.first);
                RecipeOptions options(info.recipe, kv.second.value("options", nlohmann::json::object()));
                std::optional<bool> pinned = std::nullopt;
                if (kv.second.contains("pinned") && kv.second["pinned"].is_boolean())
                    pinned = kv.second["pinned"].get<bool>();
                router->load_model(kv.first, info, options, true, true, pinned,
                                   LoadPurpose::UserInference, cancel);
                const int pid = router->loaded_model_pid(kv.first);
                std::lock_guard<std::mutex> lk(*state_mutex);
                auto it = job_states->find(job_id);
                if (it != job_states->end()) it->second.owned[kv.first] = {{"pid", pid}};
            } catch (const std::exception& e) {
                if (cancel && cancel->load()) return false;
                LOG(WARNING, "Jobs") << "could not restore job model '" << kv.first
                                     << "' on resume: " << e.what() << std::endl;
                std::lock_guard<std::mutex> lk(*state_mutex);
                auto it = job_states->find(job_id);
                if (it != job_states->end()) it->second.owned.erase(kv.first);
            }
        }
        return true;
    };
    providers.discard_exclusive = [job_states, state_mutex](const std::string& job_id) {
        std::lock_guard<std::mutex> lk(*state_mutex);
        job_states->erase(job_id);
    };
    return providers;
}

} // namespace lemon

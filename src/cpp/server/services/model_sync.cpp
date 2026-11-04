#include "lemon/server/model_sync.h"

#include "lemon/runtime_config.h"
#include "lemon/server/api_route.h"

namespace lemon {

using ordered_json = nlohmann::ordered_json;

const RouteError kOfflineUpdates{409, "", "`offline` is set to `true`; no network request is made"};

bool refuse_offline_updates(ServerContext& ctx, httplib::Response& res, const std::string& action) {
    if (!ctx.config->offline()) {
        return false;
    }
    write_plain_error(res, kOfflineUpdates, "Cannot " + action + " model updates while offline=true");
    return true;
}

ordered_json model_sync_status_schema() {
    return ordered_json::parse(R"({
        "type": "object",
        "required": ["status", "sync_id", "completed_sync_id", "already_in_progress"],
        "properties": {
            "status": {"enum": ["idle", "in_progress", "success", "failed", "not_found"], "description": "`idle` before any sync, and after a sync other than a dry run that found no models to check; `in_progress` while one runs; `success` or `failed` once it finishes; and `not_found` for an unknown `sync_id`."},
            "sync_id": {"type": "integer", "description": "The sync this status describes; `0` before any sync."},
            "completed_sync_id": {"type": "integer", "description": "The most recent sync that finished."},
            "already_in_progress": {"type": "boolean", "description": "Whether a sync this request did not start is running."},
            "dry_run": {"type": "boolean", "description": "`true` when the request was a dry run."},
            "async": {"type": "boolean", "description": "`true` when the sync was started in the background."},
            "message": {"type": "string", "description": "Present when the sync was started in the background."},
            "is_full_sync": {"type": "boolean", "description": "Whether the sync covers every downloaded model. Absent in a dry run, and when `status` is `not_found`."},
            "active_targets": {"type": "array", "items": {"type": "string"}, "description": "Models being updated now. Absent in a dry run, and when `status` is `not_found`."},
            "pending_targets": {"type": "array", "items": {"type": "string"}, "description": "Models queued for the running sync and not yet started. Absent in a dry run, and when `status` is `not_found`."},
            "completed_targets": {"type": "array", "items": {"type": "string"}, "description": "Models the sync has updated. Absent in a dry run, and when `status` is `not_found`."},
            "models_updated": {"type": "array", "items": {"type": "string"}, "description": "Models with an update; downloaded unless `dry_run`. Absent when `status` is `not_found`."},
            "models_up_to_date": {"type": "array", "items": {"type": "string"}, "description": "Models already at the latest upstream commit; absent when `status` is `not_found`."},
            "failed_models": {"type": "object", "additionalProperties": {"type": "string"}, "description": "Error message per model that could not be checked or updated; absent when `status` is `not_found`."},
            "terminal_error": {"type": "string", "description": "Error that stopped the sync; empty when none did. Absent in a dry run, and when `status` is `not_found`."},
            "checked_count": {"type": "integer", "description": "Number of models checked for updates; absent when `status` is `not_found`."},
            "updated_count": {"type": "integer", "description": "Number of models in `models_updated`; absent when `status` is `not_found`."},
            "progress": {
                "type": "object",
                "description": "While a sync runs: the file downloading now.",
                "properties": {
                    "current_model": {"type": "string", "description": "The model being updated."},
                    "file": {"type": "string", "description": "The file downloading now."},
                    "file_index": {"type": "integer", "description": "Its position among the model's files, from `1`."},
                    "total_files": {"type": "integer", "description": "How many files the model has."},
                    "bytes_downloaded": {"type": "integer", "description": "Bytes of the current file downloaded so far."},
                    "bytes_total": {"type": "integer", "description": "Size of the current file in bytes."},
                    "percent": {"type": "number", "description": "Share of the current file downloaded, from `0` to `100`."}
                }
            }
        }
    })");
}

const RouteError kLemondOffline{400, "lemond_offline", "`offline` is set to `true`"};

bool refuse_offline_download(ServerContext& ctx, httplib::Response& res) {
    if (!ctx.config->offline()) {
        return false;
    }
    write_plain_error(res, kLemondOffline, "Lemond is in offline mode, models not downloaded");
    return true;
}

} // namespace lemon

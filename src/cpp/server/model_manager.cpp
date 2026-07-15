#include <lemon/model_manager.h>
#include <lemon/runtime_config.h>
#include <lemon/hf_variants.h>
#include <lemon/model_registry.h>
#include <lemon/routing_policy_parser.h>
#include <lemon/utils/json_utils.h>
#include <lemon/utils/http_client.h>
#include <lemon/utils/process_manager.h>
#include <lemon/utils/path_utils.h>
#include <lemon/system_info.h>
#include <lemon/backends/backend_descriptor_registry.h>
#include <lemon/backends/backend_registry.h>
#include <lemon/backends/backend_utils.h>
#include <lemon/backends/cloud/cloud_server.h>
#include <lemon/backends/fastflowlm/fastflowlm_models.h>
#include <lemon/cloud_provider_registry.h>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <thread>
#include <chrono>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <iomanip>
#include <lemon/utils/aixlog.hpp>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace lemon::utils;

#ifdef _WIN32
#include <windows.h>
#include <Shlobj.h>
// MSVC's std::filesystem refuses to traverse reparse points it considers
// "untrusted" when the process token lacks symlink privileges (e.g., when
// launched from an MSI installer custom action). The Win32 API has no such
// restriction. These helpers replace fs::exists / fs::is_directory for paths
// that may contain symlinks (HuggingFace cache uses them for deduplication).
static bool safe_exists(const fs::path& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}
static bool safe_is_directory(const fs::path& p) {
    DWORD attrs = GetFileAttributesW(p.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}
// fs::recursive_directory_iterator also throws on these reparse points.
// skip_permission_denied tells it to skip inaccessible entries instead of throwing.
static constexpr auto safe_dir_options = fs::directory_options::skip_permission_denied;
// MSVC's create_directories also fails on symlinks crossing volume boundaries
// ("untrusted mount point"). SHCreateDirectoryExW does not have this restriction.
// Throws on failure to preserve the fail-fast semantics of fs::create_directories.
static void ensure_create_directories(const fs::path& p) {
    if (p.empty()) return;
    if (safe_is_directory(p)) return;
    if (safe_exists(p)) {
        throw std::runtime_error("Cannot create directory; a non-directory already exists at '" +
                                 path_to_utf8(p) + "'");
    }
    std::error_code ec;
    fs::create_directories(p, ec);
    if (!ec) return;
    // Fall back to Win32 API which handles cross-volume symlinks gracefully
    std::wstring wpath = p.wstring();
    DWORD result = SHCreateDirectoryExW(NULL, wpath.c_str(), NULL);
    if (result != ERROR_SUCCESS && result != ERROR_ALREADY_EXISTS) {
        char error_msg[256];
        FormatMessageA(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            result,
            0,
            error_msg,
            sizeof(error_msg),
            nullptr
        );
        std::string desc = error_msg[0] ? error_msg : "unknown error";
        throw std::runtime_error("Failed to create directory '" + path_to_utf8(p) +
                                 "': " + desc);
    }
}
#else
static bool safe_exists(const fs::path& p) { return fs::exists(p); }
static bool safe_is_directory(const fs::path& p) { return fs::is_directory(p); }
static void ensure_create_directories(const fs::path& p) {
    if (p.empty()) return;
    if (safe_is_directory(p)) return;
    if (safe_exists(p)) {
        throw std::runtime_error("Cannot create directory; a non-directory already exists at '" +
                                 path_to_utf8(p) + "'");
    }
    std::error_code ec;
    fs::create_directories(p, ec);
    if (ec) {
        throw std::runtime_error("Failed to create directory '" + path_to_utf8(p) +
                                 "': " + ec.message());
    }
}
static constexpr auto safe_dir_options = fs::directory_options::none;
#endif

namespace lemon {

// Properties which are defined by the user for model registration.
static const std::vector<std::string> USER_DEFINED_MODEL_PROPS = std::vector<std::string>{"checkpoints", "checkpoint", "recipe", "mmproj", "size", "image_defaults", "components", "recipe_options", "routing", "system_prompt", "version", "source", "registry_source"};

static constexpr const char USER_MODEL_PREFIX[] = "user.";
static constexpr size_t USER_MODEL_PREFIX_LEN = sizeof(USER_MODEL_PREFIX) - 1;
static constexpr const char EXTRA_MODEL_PREFIX[] = "extra.";

// Built-ins are keyed bare in models_cache_; user.* and extra.* keys already
// include their canonical prefix. This helper returns the canonical ID for any
// cache key, which is the form used by recipe_options.json on disk.
static std::string cache_key_to_canonical_id(const std::string& cache_key) {
    if (parse_canonical_id(cache_key)) {
        return cache_key;
    }
    return canonical_id(ModelSource::Builtin, cache_key);
}

// Candidate roots that FLM may use to store models. FLM resolves its model
// directory from the FLM_MODEL_PATH env var (set by the installer) and falls
// back to a built-in default that has changed across releases. lemond is often
// launched from a parent process that predates the FLM install and therefore
// doesn't see FLM_MODEL_PATH, so we also probe every documented default.
// Order is most-specific to most-historical.

static void populate_model_metadata(ModelInfo& info) {
    info.max_context_window = 0;
    if (!info.downloaded) return;

    // Per-backend metadata (GGUF arch/labels for llamacpp, config.json ctx for
    // flm, …) is read by the backend's ops, not a recipe switchboard here.
    backends::BackendOpsContext ctx;
    backends::ops_for(info.recipe)->populate_metadata(info, ctx);
}

static bool is_user_model_name(const std::string& model_name) {
    return model_name.rfind(USER_MODEL_PREFIX, 0) == 0;
}

static bool is_extra_model_name(const std::string& model_name) {
    return model_name.rfind(EXTRA_MODEL_PREFIX, 0) == 0;
}

static std::string strip_user_model_prefix(const std::string& model_name) {
    if (is_user_model_name(model_name)) {
        return model_name.substr(USER_MODEL_PREFIX_LEN);
    }
    return model_name;
}

static std::string effective_registry_source(const ModelInfo& info) {
    return remote_registry_source_name(parse_remote_registry_source(info.registry_source));
}

static void parse_model_source_fields(ModelInfo& info, const json& model_json) {
    const std::string raw_source = JsonUtils::get_or_default<std::string>(model_json, "source", "");
    const std::string explicit_registry = JsonUtils::get_or_default<std::string>(
        model_json, "registry_source", "");

    std::string normalized_explicit;
    if (!explicit_registry.empty()) {
        normalized_explicit = remote_registry_source_name(
            parse_remote_registry_source(explicit_registry));
        info.registry_source = normalized_explicit;
    }
    if (is_remote_registry_source(raw_source)) {
        const std::string normalized_public = remote_registry_source_name(
            parse_remote_registry_source(raw_source));
        if (!normalized_explicit.empty() && normalized_explicit != normalized_public) {
            throw std::invalid_argument(
                "Model source and registry_source identify different registries");
        }
        info.registry_source = normalized_public;
        info.source.clear();
    } else {
        info.source = raw_source;
    }
}

static std::string repo_id_to_cache_dir_name(const std::string& repo_id,
                                             const std::string& registry_source = "huggingface") {
    return registry_repo_cache_dir_name(repo_id,
        parse_remote_registry_source(registry_source));
}

static std::string read_hf_ref_main(const fs::path& model_cache_path) {
    fs::path refs_main_path = model_cache_path / "refs" / "main";
    std::ifstream refs_file(refs_main_path);
    if (!refs_file.is_open()) {
        return "";
    }

    std::string ref;
    std::getline(refs_file, ref);
    ref.erase(0, ref.find_first_not_of(" \t\r\n"));
    size_t last = ref.find_last_not_of(" \t\r\n");
    if (last == std::string::npos) {
        return "";
    }
    ref.erase(last + 1);
    return ref;
}

static fs::path active_hf_snapshot_path(const fs::path& model_cache_path) {
    std::string ref = read_hf_ref_main(model_cache_path);
    if (ref.empty()) {
        return fs::path();
    }

    fs::path snapshot_path = model_cache_path / "snapshots" / ref;
    return safe_exists(snapshot_path) ? snapshot_path : fs::path();
}

static void write_hf_ref_main(const fs::path& model_cache_path, const std::string& commit_hash) {
    if (commit_hash.empty()) {
        return;
    }

    fs::path refs_dir = model_cache_path / "refs";
    ensure_create_directories(refs_dir);
    std::ofstream refs_file(refs_dir / "main");
    if (refs_file.is_open()) {
        refs_file << commit_hash;
    }
}

static std::string checkpoint_to_repo_id(std::string checkpoint) {
    std::string repo_id = checkpoint;

    size_t colon_pos = checkpoint.find(':');
    if (colon_pos != std::string::npos) {
        repo_id = repo_id.substr(0, colon_pos);
    }

    return repo_id;
}

static std::string checkpoint_to_variant(std::string checkpoint) {
    std::string variant = "";

    size_t colon_pos = checkpoint.find(':');
    if (colon_pos != std::string::npos) {
        variant = checkpoint.substr(colon_pos + 1);
    }

    return variant;
}

// Check if any model other than exclude_model references the given repo_id
static bool is_repo_shared(const std::string& repo_id,
                           const std::string& registry_source,
                           const std::string& exclude_model,
                           const std::map<std::string, ModelInfo>& cache) {
    const std::string normalized_source = remote_registry_source_name(
        parse_remote_registry_source(registry_source));
    for (const auto& [name, info] : cache) {
        if (name == exclude_model || !info.source.empty()) continue;
        if (effective_registry_source(info) != normalized_source) continue;
        for (const auto& [type, cp] : info.checkpoints) {
            (void)type;
            if (checkpoint_to_repo_id(cp) == repo_id) return true;
        }
    }
    return false;
}

// Parse image_defaults from a model JSON entry into ModelInfo
static void parse_image_defaults(ModelInfo& info, const json& model_json) {
    if (model_json.contains("image_defaults") && model_json["image_defaults"].is_object()) {
        const auto& img_defaults = model_json["image_defaults"];
        info.image_defaults.has_defaults = true;
        info.image_defaults.steps = JsonUtils::get_or_default<int>(img_defaults, "steps", 20);
        info.image_defaults.cfg_scale = JsonUtils::get_or_default<float>(img_defaults, "cfg_scale", 7.0f);
        info.image_defaults.width = JsonUtils::get_or_default<int>(img_defaults, "width", 512);
        info.image_defaults.height = JsonUtils::get_or_default<int>(img_defaults, "height", 512);
        info.image_defaults.sampling_method = JsonUtils::get_or_default<std::string>(img_defaults, "sampling_method", "");
        info.image_defaults.flow_shift = JsonUtils::get_or_default<float>(img_defaults, "flow_shift", 0.0f);
    }
}

// Populate ModelInfo::extras with any model-JSON key not consumed by a typed
// ModelInfo field. This lets a new backend read custom per-model fields in load()
// without editing the shared ModelInfo struct. Keep this set in sync with the
// keys read by the parse blocks in build_cache().
static void parse_extras(ModelInfo& info, const json& model_json) {
    static const std::set<std::string> kKnownKeys = {
        "checkpoint", "checkpoints", "components", "mmproj", "recipe", "suggested",
        "source", "registry_source", "size", "cloud_provider",
        "labels", "image_defaults", "recipe_options"
    };
    if (!model_json.is_object()) return;
    for (auto& [key, value] : model_json.items()) {
        if (kKnownKeys.count(key) == 0) {
            info.extras[key] = value;
        }
    }
}

// Default device for a recipe: the backend descriptor is authoritative for
// registered backends; collection/unknown recipes fall back to the recipe map.
// (A backend whose device depends on the chosen backend variant resolves the
// final device at load time via WrappedServer::effective_device.)
static DeviceType device_type_for_recipe(const std::string& recipe) {
    if (const auto* desc = lemon::backends::descriptor_for(recipe)) {
        return desc->default_device;
    }
    return get_device_type_from_recipe(recipe);
}

// Build merged recipe options: image_defaults -> JSON recipe_options -> user-saved overrides.
// json_recipe_options: pre-extracted recipe_options for this model (from build_cache's
// two-phase pattern). Pass a null json if the model JSON should be read directly instead.
// saved_recipe_options_key: canonical ID (user.* / extra.* / builtin.*) under which the
// user's saved options are keyed in recipe_options.json. Translate from the cache key with
// cache_key_to_canonical_id() before calling.
static RecipeOptions build_recipe_options(const ModelInfo& info,
                                          const json& json_recipe_options,
                                          const std::string& saved_recipe_options_key,
                                          const json& saved_recipe_options) {
    json base_options = json::object();

    // Layer 1: image_defaults as base
    if (info.image_defaults.has_defaults) {
        base_options["steps"] = info.image_defaults.steps;
        base_options["cfg_scale"] = info.image_defaults.cfg_scale;
        base_options["width"] = info.image_defaults.width;
        base_options["height"] = info.image_defaults.height;
        if (!info.image_defaults.sampling_method.empty())
            base_options["sampling_method"] = info.image_defaults.sampling_method;
        if (info.image_defaults.flow_shift > 0.0f)
            base_options["flow_shift"] = info.image_defaults.flow_shift;
    }

    // Layer 2: JSON-level recipe_options override image_defaults (e.g. sdcpp_args)
    if (!json_recipe_options.is_null() && json_recipe_options.is_object()) {
        for (auto& [key, value] : json_recipe_options.items()) {
            base_options[key] = value;
        }
    }

    // Layer 3: User-saved recipe options override everything
    if (JsonUtils::has_key(saved_recipe_options, saved_recipe_options_key)) {
        auto saved = saved_recipe_options[saved_recipe_options_key];
        for (auto& [key, value] : saved.items()) {
            base_options[key] = value;
        }
    }

    return RecipeOptions(info.recipe, base_options);
}

// Clean up orphaned HF cache blobs after deleting a symlink.
// HF hub downloads use: snapshots/<hash>/file.gguf -> ../../blobs/<sha256>
// If no remaining symlink in the repo points to the blob, it's safe to remove.
//
// TODO: A more robust approach would be to extend cleanup_orphaned_cache()
// to scan for orphaned blobs across all repos, triggered automatically after
// delete. This would need integration tests that use huggingface_hub Python
// tooling (e.g. hf_hub_download()) to create the blob+symlink layout, since
// Lemonade's own downloader writes real files without blobs.
static void cleanup_orphaned_blob(const fs::path& file_path,
                                  const fs::path& models_dir) {
    std::error_code ec;
    if (!fs::is_symlink(file_path, ec) || ec) {
        return;  // Not a symlink (real file) or error — nothing to clean up
    }

    // Resolve the blob target (relative symlink like ../../blobs/<hash>)
    fs::path link_target = fs::read_symlink(file_path, ec);
    if (ec) return;
    fs::path blob_path = fs::canonical(file_path.parent_path() / link_target, ec);
    if (ec || !fs::exists(blob_path)) return;

    // Check if any other symlink in the repo still references this blob
    fs::path snapshots_dir = models_dir / "snapshots";
    if (!fs::exists(snapshots_dir)) return;

    for (auto& entry : fs::recursive_directory_iterator(snapshots_dir, ec)) {
        if (ec) break;
        if (entry.path() == file_path) continue;  // Skip the file we're about to delete
        if (!fs::is_symlink(entry.path(), ec) || ec) continue;

        fs::path other_target = fs::read_symlink(entry.path(), ec);
        if (ec) continue;
        fs::path other_blob = fs::canonical(entry.path().parent_path() / other_target, ec);
        if (!ec && other_blob == blob_path) {
            // Another symlink still references this blob — keep it
            return;
        }
    }

    // No other symlink references this blob — safe to remove
    LOG(INFO, "ModelManager") << "Removing orphaned blob: " << path_to_utf8(blob_path) << std::endl;
    fs::remove(blob_path, ec);

    // Clean up empty blobs/ directory
    fs::path blobs_dir = blob_path.parent_path();
    if (fs::exists(blobs_dir) && fs::is_empty(blobs_dir, ec) && !ec) {
        fs::remove(blobs_dir, ec);
    }
}

// Remove empty parent directories up to (but not including) the stop directory
static void cleanup_empty_parents(const fs::path& file_path, const fs::path& stop_dir) {
    std::error_code ec;
    fs::path parent = file_path.parent_path();
    for (int i = 0; i < 6 && !parent.empty() && parent != stop_dir; ++i) {
        if (fs::is_empty(parent, ec) && !ec) {
            fs::remove(parent, ec);
            parent = parent.parent_path();
        } else {
            break;
        }
    }
}

// Return the on-disk size of a resolved model path. Some recipes (for
// example Moonshine streaming) resolve to a directory of artifacts rather than
// to a single model file. std::filesystem::file_size() fails on directories
static uintmax_t resolved_path_size_bytes(const fs::path& path) {
    std::error_code ec;
    if (!safe_exists(path)) {
        return 0;
    }

    if (!safe_is_directory(path)) {
        auto size = fs::file_size(path, ec);
        return ec ? 0 : size;
    }

    uintmax_t total = 0;
    for (const auto& entry : fs::recursive_directory_iterator(path, safe_dir_options, ec)) {
        if (ec) {
            ec.clear();
            break;
        }
        if (!entry.is_regular_file(ec)) {
            ec.clear();
            continue;
        }

        auto size = fs::file_size(entry.path(), ec);
        if (!ec) {
            total += size;
        } else {
            ec.clear();
        }
    }
    return total;
}


// Replace the static registry size with the aggregate on-disk size once the
// files exist, so directory-checkpoint models (whose repos can carry more than
// the registry estimate) report what was actually downloaded.
static void refresh_on_disk_size(ModelInfo& info) {
    uintmax_t total_size = 0;
    for (auto& [type, path] : info.resolved_paths) {
        (void)type;
        total_size += resolved_path_size_bytes(path_from_utf8(path));
    }
    if (total_size == 0) {
        return;
    }
    double file_size_gb = static_cast<double>(total_size) / (1024.0 * 1024.0 * 1024.0);
    if (file_size_gb < 1.0) {
        info.size = std::round(file_size_gb * 1000) / 1000;
    } else if (file_size_gb < 10.0) {
        info.size = std::round(file_size_gb * 100) / 100;
    } else {
        info.size = std::round(file_size_gb * 10) / 10;
    }
}

std::vector<ModelFileInfo> ModelManager::list_model_files(const std::string& model_name) {
    ModelInfo info = get_model_info(model_name);
    std::vector<ModelFileInfo> files;
    files.reserve(info.resolved_paths.size());

    for (const auto& [role, resolved_path] : info.resolved_paths) {
        if (resolved_path.empty()) {
            continue;
        }

        fs::path path = path_from_utf8(resolved_path);
        const bool path_exists = safe_exists(path);

        ModelFileInfo file;
        file.name = path_to_utf8(path.filename());
        file.path = resolved_path;
        file.role = role;
        file.exists = path_exists;
        file.size_bytes = path_exists
            ? static_cast<std::uint64_t>(resolved_path_size_bytes(path))
            : 0;
        files.push_back(std::move(file));
    }

    return files;
}

static void cleanup_orphaned_blobs_under(const fs::path& path,
                                         const fs::path& models_dir) {
    if (!safe_exists(path)) {
        return;
    }

    if (!safe_is_directory(path)) {
        cleanup_orphaned_blob(path, models_dir);
        return;
    }

    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(path, safe_dir_options, ec)) {
        if (ec) {
            ec.clear();
            break;
        }
        cleanup_orphaned_blob(entry.path(), models_dir);
    }
}

static void remove_resolved_path_or_throw(const fs::path& path,
                                          const std::string& description) {
    if (!safe_exists(path)) {
        return;
    }

    LOG(INFO, "ModelManager") << "Removing " << description << ": "
                              << path_to_utf8(path) << std::endl;

    std::error_code ec;
    if (safe_is_directory(path)) {
        fs::remove_all(path, ec);
    } else {
        fs::remove(path, ec);
    }

    if (ec) {
        throw std::runtime_error("Failed to remove " + description + " '" +
                                 path_to_utf8(path) + "': " + ec.message());
    }
}


static std::string normalized_relative_path(const fs::path& path, const fs::path& root) {
    std::string rel = path_to_utf8(path.lexically_relative(root));
    std::replace(rel.begin(), rel.end(), '\\', '/');
    return rel;
}

static void remove_file_or_throw(const fs::path& path, const std::string& description) {
    if (!safe_exists(path)) {
        return;
    }

    LOG(INFO, "ModelManager") << "Removing " << description << ": "
                              << path_to_utf8(path) << std::endl;
    std::error_code ec;
    fs::remove(path, ec);
    if (ec) {
        throw std::runtime_error("Failed to remove " + description + " '" +
                                 path_to_utf8(path) + "': " + ec.message());
    }
}

static void remove_stale_manifest_for_partial(const fs::path& partial_path,
                                              const fs::path& stop_dir) {
    fs::path parent = partial_path.parent_path();
    for (int i = 0; i < 8 && !parent.empty() && parent != stop_dir; ++i) {
        fs::path manifest_path = parent / ".download_manifest.json";
        if (safe_exists(manifest_path)) {
            remove_file_or_throw(manifest_path, "stale download manifest");
            cleanup_empty_parents(manifest_path, stop_dir);
            return;
        }
        parent = parent.parent_path();
    }
}

static bool cleanup_incomplete_hf_model_cache(const ModelInfo& info,
                                              const std::string& canonical_model_name,
                                              const std::map<std::string, ModelInfo>& models_cache) {
    const std::string main_checkpoint = info.checkpoint("main");
    const std::string main_repo = checkpoint_to_repo_id(main_checkpoint);
    if (main_repo.empty()) {
        return false;
    }

    fs::path model_cache_path = path_from_utf8(get_hf_cache_dir()) / repo_id_to_cache_dir_name(main_repo, effective_registry_source(info));
    if (!safe_exists(model_cache_path)) {
        return false;
    }

    // If no other model references this HF repo, delete the whole incomplete
    // repo cache. That removes .partial files, stale manifests, any files that
    // finished before cancellation, refs, and blobs in one atomic intent.
    if (!is_repo_shared(main_repo, effective_registry_source(info), canonical_model_name, models_cache)) {
        LOG(INFO, "ModelManager") << "Removing incomplete model cache: "
                                  << path_to_utf8(model_cache_path) << std::endl;
        std::error_code ec;
        fs::remove_all(model_cache_path, ec);
        if (ec) {
            throw std::runtime_error("Failed to remove incomplete model cache '" +
                                     path_to_utf8(model_cache_path) + "': " + ec.message());
        }
        return true;
    }

    // Shared repos must not be removed wholesale. Remove only the resumable
    // partial for this model's requested variant, plus the manifest that made
    // that partial visible as an incomplete download.
    const std::string variant = checkpoint_to_variant(main_checkpoint);
    if (variant.empty()) {
        LOG(INFO, "ModelManager") << "Keeping shared incomplete cache for " << main_repo
                                  << " because the model has no exact variant" << std::endl;
        return false;
    }

    fs::path snapshots_dir = model_cache_path / "snapshots";
    if (!safe_exists(snapshots_dir)) {
        return false;
    }

    std::string normalized_variant = variant;
    std::replace(normalized_variant.begin(), normalized_variant.end(), '\\', '/');

    bool removed_any = false;
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(snapshots_dir, safe_dir_options, ec)) {
        if (ec) break;

        const fs::path partial_path = entry.path();
        const std::string rel = normalized_relative_path(partial_path, snapshots_dir);
        const std::string filename = partial_path.filename().string();
        const bool matches_variant_partial =
            filename == variant + ".partial" ||
            rel == normalized_variant + ".partial" ||
            rel.rfind("/" + normalized_variant + ".partial") != std::string::npos;

        if (!matches_variant_partial) {
            continue;
        }

        remove_file_or_throw(partial_path, "incomplete model download partial");
        remove_stale_manifest_for_partial(partial_path, model_cache_path);
        cleanup_empty_parents(partial_path, model_cache_path);
        removed_any = true;
    }

    if (!removed_any) {
        LOG(INFO, "ModelManager") << "No incomplete cache artifacts found for shared repo "
                                  << main_repo << std::endl;
    }

    return removed_any;
}

// Structure to hold identified GGUF files
struct GGUFFiles {
    std::map<std::string, std::string> core_files;  // {"variant": "file.gguf", "mmproj": "file.mmproj"}
    std::vector<std::string> sharded_files;         // Additional shard files
};

// Identifies GGUF model files matching the variant (Python equivalent of identify_gguf_models)
static GGUFFiles identify_gguf_models(
    const std::string& checkpoint,
    const std::string& variant,
    const std::vector<std::string>& repo_files
) {
    const std::string hint = R"(
    The CHECKPOINT:VARIANT scheme is used to specify model files in remote registry repositories.

    The VARIANT format can be one of several types:
    0. wildcard (*): download all .gguf files in the repo
    1. Full filename: exact file to download
    2. None/empty: gets the first .gguf file in the repository (excludes mmproj files)
    3. Quantization variant: find a single file ending with the variant name (case insensitive)
    4. Folder name: downloads all .gguf files in the folder that matches the variant name (case insensitive)

    Examples:
    - "ggml-org/gpt-oss-120b-GGUF:*" -> downloads all .gguf files in repo
    - "unsloth/Qwen3-8B-GGUF:qwen3.gguf" -> downloads "qwen3.gguf"
    - "unsloth/Qwen3-30B-A3B-GGUF" -> downloads "Qwen3-30B-A3B-GGUF.gguf"
    - "unsloth/Qwen3-8B-GGUF:Q4_1" -> downloads "Qwen3-8B-GGUF-Q4_1.gguf"
    - "unsloth/Qwen3-30B-A3B-GGUF:Q4_0" -> downloads all files in "Q4_0/" folder
    )";

    GGUFFiles result;
    std::vector<std::string> sharded_files;
    std::string variant_name;
    auto join_files = [](const std::vector<std::string>& files) {
        std::string out;
        for (size_t i = 0; i < files.size(); ++i) {
            if (i > 0) out += ", ";
            out += files[i];
        }
        return out;
    };

    // (case 0) Wildcard, download everything
    if (!variant.empty() && variant == "*") {
        for (const auto& f : repo_files) {
            if (gguf_reader_detail::ends_with_ignore_case(f, ".gguf")) {
                sharded_files.push_back(f);
            }
        }

        if (sharded_files.empty()) {
            throw std::runtime_error("No .gguf files found in repository " + checkpoint + ". " + hint);
        }

        // Sort to ensure consistent ordering
        std::sort(sharded_files.begin(), sharded_files.end());

        // Use first file as primary (this is how llamacpp handles it)
        variant_name = sharded_files[0];
    }
    // (case 1) If variant ends in .gguf or .bin, use it directly
    else if (!variant.empty() && (gguf_reader_detail::ends_with_ignore_case(variant, ".gguf") || gguf_reader_detail::ends_with_ignore_case(variant, ".bin"))) {
        variant_name = variant;

        // Validate file exists in repo
        bool found = false;
        for (const auto& f : repo_files) {
            if (f == variant) {
                found = true;
                break;
            }
        }

        if (!found) {
            throw std::runtime_error(
                "File " + variant + " not found in remote model repository " + checkpoint + ". " + hint
            );
        }
    }
    // (case 2) If no variant is provided, get the first .gguf file in the repository
    else if (variant.empty()) {
        std::vector<std::string> all_variants;
        for (const auto& f : repo_files) {
            if (gguf_reader_detail::ends_with_ignore_case(f, ".gguf") && !gguf_reader_detail::contains_ignore_case(f, "mmproj")) {
                all_variants.push_back(f);
            }
        }

        if (all_variants.empty()) {
            throw std::runtime_error(
                "No .gguf files found in remote model repository " + checkpoint + ". " + hint
            );
        }

        variant_name = all_variants[0];
    }
    else {
        auto vset = lemon::enumerate_gguf_variants(repo_files);
        std::vector<lemon::GgufVariant> exact_matches;
        for (const auto& v : vset.variants) {
            if (gguf_reader_detail::to_lower(v.name) == gguf_reader_detail::to_lower(variant)) {
                exact_matches.push_back(v);
            }
        }

        if (exact_matches.size() == 1) {
            variant_name = exact_matches[0].primary_file;
            sharded_files = exact_matches[0].files;
        } else if (exact_matches.size() > 1) {
            std::vector<std::string> matching_files;
            for (const auto& match : exact_matches) {
                matching_files.insert(matching_files.end(), match.files.begin(), match.files.end());
            }
            std::sort(matching_files.begin(), matching_files.end());
            throw std::runtime_error(
                "Multiple GGUF variant groups matched '" + variant + "': " +
                join_files(matching_files) + ". " + hint
            );
        } else {
            // Fallback for repos that require direct filename matching rather than
            // the canonical variant set used by /pull/variants.
            std::vector<std::string> end_with_variant;
            std::string variant_suffix = variant + ".gguf";

            for (const auto& f : repo_files) {
                if (gguf_reader_detail::ends_with_ignore_case(f, variant_suffix) && !gguf_reader_detail::contains_ignore_case(f, "mmproj")) {
                    end_with_variant.push_back(f);
                }
            }

            if (end_with_variant.size() == 1) {
                variant_name = end_with_variant[0];
            }
            else if (end_with_variant.size() > 1) {
                std::sort(end_with_variant.begin(), end_with_variant.end());
                throw std::runtime_error(
                    "Multiple .gguf files matched variant '" + variant + "': " +
                    join_files(end_with_variant) + ". " + hint
                );
            }
            // (case 4) Check whether the variant corresponds to a folder with sharded files (case insensitive)
            else {
                std::string folder_prefix = variant + "/";
                for (const auto& f : repo_files) {
                    if (gguf_reader_detail::ends_with_ignore_case(f, ".gguf") && gguf_reader_detail::starts_with_ignore_case(f, folder_prefix)) {
                        sharded_files.push_back(f);
                    }
                }

                // If no exact folder match, try folders ending with -{variant}/ or _{variant}/
                // This handles repos where the folder is prefixed with the model name,
                // e.g. "Qwen3-Coder-Next-Q4_K_M/" instead of just "Q4_K_M/"
                if (sharded_files.empty()) {
                    std::string suffix_dash = "-" + variant + "/";
                    std::string suffix_underscore = "_" + variant + "/";
                    for (const auto& f : repo_files) {
                        if (!gguf_reader_detail::ends_with_ignore_case(f, ".gguf")) continue;
                        size_t slash_pos = f.find('/');
                        if (slash_pos != std::string::npos) {
                            std::string folder = f.substr(0, slash_pos + 1);
                            if (gguf_reader_detail::ends_with_ignore_case(folder, suffix_dash) ||
                                gguf_reader_detail::ends_with_ignore_case(folder, suffix_underscore)) {
                                sharded_files.push_back(f);
                            }
                        }
                    }
                }

                if (sharded_files.empty()) {
                    throw std::runtime_error(
                        "No .gguf files found for variant " + variant + ". " + hint
                    );
                }

                // Sort to ensure consistent ordering
                std::sort(sharded_files.begin(), sharded_files.end());

                // Use first file as primary (this is how llamacpp handles it)
                variant_name = sharded_files[0];
            }
        }
    }

    result.core_files["variant"] = variant_name;
    result.sharded_files = sharded_files;

    return result;
}

ModelManager::ModelManager(const std::string& extra_models_dir)
    : extra_models_dir_(extra_models_dir) {
    server_models_ = load_server_models();
    user_models_ = load_optional_json(get_user_models_file());
    recipe_options_ = load_optional_json(get_recipe_options_file());
    architecture_defaults_ = load_architecture_defaults();

    // One-shot migration of recipe_options.json: older Lemonade keyed built-in
    // entries by bare name; the current spec requires a canonical "builtin."
    // prefix so each source is addressable distinctly even on shadowing.
    //
    // Normalize to an object before iterating — a corrupted file may parse as
    // null, array, or scalar, and json::iterator::key() throws on non-objects.
    if (!recipe_options_.is_object()) {
        if (!recipe_options_.is_null()) {
            LOG(WARNING, "ModelManager") << "recipe_options.json is not a JSON object; resetting to empty object" << std::endl;
        }
        recipe_options_ = json::object();
    }
    {
        int migrated = 0;
        json migrated_options = json::object();
        for (auto it = recipe_options_.begin(); it != recipe_options_.end(); ++it) {
            const std::string& key = it.key();
            if (parse_canonical_id(key)) {
                migrated_options[key] = it.value();
            } else if (server_models_.contains(key)) {
                migrated_options[canonical_id(ModelSource::Builtin, key)] = it.value();
                ++migrated;
            } else {
                // Preserve unknown bare keys (likely stale built-ins) — avoids silent data loss.
                migrated_options[key] = it.value();
            }
        }
        if (migrated > 0) {
            recipe_options_ = std::move(migrated_options);
            try {
                fs::path dir = fs::path(get_recipe_options_file()).parent_path();
                ensure_create_directories(dir);
                JsonUtils::save_to_file(recipe_options_, get_recipe_options_file());
                LOG(INFO, "ModelManager") << "migrated " << migrated
                          << " legacy recipe_options keys to builtin. prefix" << std::endl;
            } catch (const std::exception& e) {
                LOG(WARNING, "ModelManager") << "Could not persist migrated recipe_options.json: "
                                              << e.what() << std::endl;
            }
        }
    }

    if (!extra_models_dir_.empty()) {
        LOG(INFO, "ModelManager") << "Extra models directory set to: " << extra_models_dir_ << std::endl;
        start_directory_watcher();
    }
}

std::string ModelManager::get_user_models_file() {
    return get_cache_dir() + "/user_models.json";
}

std::string ModelManager::get_recipe_options_file() {
    return get_cache_dir() + "/recipe_options.json";
}

std::string ModelManager::get_hf_cache_dir() const {
    return lemon::utils::get_hf_cache_dir();
}

void ModelManager::invalidate_models_cache() {
    std::lock_guard<std::mutex> lock(models_cache_mutex_);
    cache_valid_ = false;
}

bool ModelManager::refresh_user_models_from_disk_for_lookup(const std::string& model_name) {
    std::vector<std::string> candidate_keys;

    if (auto canon = parse_canonical_id(model_name)) {
        if (canon->source == ModelSource::Registered) {
            candidate_keys.push_back(canon->bare_name);
        }
    } else if (!model_name.empty()) {
        candidate_keys.push_back(model_name);
    }

    if (candidate_keys.empty()) {
        return false;
    }

    json latest_user_models = load_optional_json(get_user_models_file());
    if (!latest_user_models.is_object()) {
        return false;
    }

    bool found = false;
    for (const auto& key : candidate_keys) {
        if (latest_user_models.contains(key)) {
            found = true;
            break;
        }
    }

    if (!found) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(models_cache_mutex_);
        user_models_ = std::move(latest_user_models);
        cache_valid_ = false;
    }

    build_cache();
    return true;
}

void ModelManager::set_extra_models_dir(const std::string& dir) {
    extra_models_dir_ = dir;

    directory_watcher_.reset();

    if (!extra_models_dir_.empty()) {
        LOG(INFO, "ModelManager") << "Extra models directory set to: " << extra_models_dir_ << std::endl;
        start_directory_watcher();
    }

    std::lock_guard<std::mutex> lock(models_cache_mutex_);
    cache_valid_ = false;
}

void ModelManager::start_directory_watcher() {
    directory_watcher_ = std::make_unique<DirectoryWatcher>(extra_models_dir_);
    directory_watcher_->set_callback([this]() {
        LOG(DEBUG, "ModelManager") << "Extra models directory changed, invalidating cache" << std::endl;
        {
            std::lock_guard<std::mutex> lock(models_cache_mutex_);
            cache_valid_ = false;
        }
    });
    directory_watcher_->start();
}

std::map<std::string, ModelInfo> ModelManager::discover_extra_models() const {
    std::map<std::string, ModelInfo> discovered;

    // If no extra models directory configured, return empty
    if (extra_models_dir_.empty()) {
        return discovered;
    }

    if (!fs::exists(extra_models_dir_)) {
        // Directory doesn't exist, return empty
        return discovered;
    }

    std::string search_dir = extra_models_dir_;

    LOG(INFO, "ModelManager") << "Scanning for GGUF models in: " << search_dir << std::endl;

    // Configuration for discovered models (single source of truth)
    static constexpr const char* EXTRA_MODEL_PREFIX = "extra.";
    static constexpr const char* EXTRA_MODEL_RECIPE = "llamacpp";
    static constexpr const char* EXTRA_MODEL_SOURCE = "extra_models_dir";

    // Helper to initialize common ModelInfo fields for discovered models
    auto init_extra_model_info = [](const std::string& name) -> ModelInfo {
        ModelInfo info;
        info.model_name = name;
        info.recipe = EXTRA_MODEL_RECIPE;
        info.suggested = true;
        info.downloaded = true;
        info.source = EXTRA_MODEL_SOURCE;
        info.labels.push_back("custom");
        info.device = device_type_for_recipe(EXTRA_MODEL_RECIPE);
        return info;
    };

    // Track which directories we've processed (for multimodal/multi-shard detection)
    std::map<std::string, std::vector<fs::path>> dirs_with_gguf;  // directory -> list of gguf files
    std::vector<fs::path> standalone_files;  // GGUF files not in subdirectories

    // Recursively find all .gguf files
    try {
        for (const auto& entry : fs::recursive_directory_iterator(search_dir)) {
            if (!entry.is_regular_file()) continue;

            std::string filename = entry.path().filename().string();

            if (!gguf_reader_detail::ends_with_ignore_case(filename, ".gguf")) continue;

            fs::path parent_dir = entry.path().parent_path();

            // Check if this file is directly in the search directory or in a subdirectory
            if (parent_dir == fs::path(search_dir)) {
                // Standalone file in the root of search directory
                standalone_files.push_back(entry.path());
            } else {
                // File in a subdirectory - group by parent directory
                dirs_with_gguf[parent_dir.string()].push_back(entry.path());
            }
        }
    } catch (const std::exception& e) {
        LOG(ERROR, "ModelManager") << "Error scanning directory " << search_dir << ": " << e.what() << std::endl;
        return discovered;
    }

    // Process standalone files (single-file models)
    for (const auto& gguf_path : standalone_files) {
        std::string filename = gguf_path.filename().string();

        // Skip mmproj files - they're part of multimodal models
        if (gguf_reader_detail::contains_ignore_case(filename, "mmproj")) continue;

        std::string model_name = std::string(EXTRA_MODEL_PREFIX) + gguf_path.stem().string();
        ModelInfo info = init_extra_model_info(model_name);
        info.checkpoints["main"] = gguf_path.string();
        info.resolved_paths["main"] = gguf_path.string();
        info.type = ModelType::LLM;

        // Calculate size in GB
        try {
            uintmax_t file_size = fs::file_size(gguf_path);
            info.size = static_cast<double>(file_size) / (1024.0 * 1024.0 * 1024.0);
        } catch (...) {
            info.size = 0.0;
        }

        discovered[model_name] = info;
    }

    // Process directories (multimodal and multi-shard models)
    for (const auto& [dir_path, gguf_files] : dirs_with_gguf) {
        if (gguf_files.empty()) continue;

        fs::path dir = fs::path(dir_path);
        std::string dir_name = dir.filename().string();

        // Find the main model file and mmproj file
        fs::path main_model_path;
        fs::path mmproj_file;
        double total_size = 0.0;

        for (const auto& gguf_path : gguf_files) {
            // Calculate total size
            try {
                uintmax_t file_size = fs::file_size(gguf_path);
                total_size += static_cast<double>(file_size) / (1024.0 * 1024.0 * 1024.0);
            } catch (...) {}

            // Check if this is an mmproj file (can be anywhere in filename)
            if (gguf_reader_detail::contains_ignore_case(gguf_path.filename().string(), "mmproj")) {
                mmproj_file = gguf_path;
                continue;
            }

            // This is a model file - for sharded models, we want the first shard
            // For non-sharded, this is the only model file
            if (main_model_path.empty() || gguf_path < main_model_path) {
                main_model_path = gguf_path;
            }
        }

        if (main_model_path.empty()) {
            // No main model file found (only mmproj?), skip
            continue;
        }

        std::string model_name = std::string(EXTRA_MODEL_PREFIX) + dir_name;
        ModelInfo info = init_extra_model_info(model_name);
        info.checkpoints["main"] = dir_path;
        info.resolved_paths["main"] = main_model_path.string();
        info.size = total_size;

        // If mmproj found, set it and add vision label
        if (!mmproj_file.empty()) {
            info.checkpoints["mmproj"] = mmproj_file.filename().string();
            info.resolved_paths["mmproj"] = mmproj_file.string();
            info.labels.push_back("vision");
        }

        info.type = get_model_type_from_labels(info.labels);

        discovered[model_name] = info;
    }

    LOG(INFO, "ModelManager") << "Discovered " << discovered.size() << " models from extra directory" << std::endl;

    return discovered;
}

std::string ModelManager::resolve_model_path(const ModelInfo& info, const std::string& type, const std::string& checkpoint) const {
    // Collections are virtual entries with no direct checkpoint to resolve.
    if (is_model_collection_recipe(info.recipe)) {
        return "";
    }

    // Local-path models use the checkpoint as-is (absolute path to a file).
    if (info.source == "local_path") {
        return checkpoint;
    }

    std::string hf_cache = get_hf_cache_dir();

    // Local uploads: checkpoint is a relative path from the HF cache.
    if (info.source == "local_upload") {
        std::string normalized = checkpoint;
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        return hf_cache + "/" + normalized;
    }

    // Compute the HF cache location for this checkpoint's repo, then let the
    // backend's ops find its artifact within (a .gguf file, a genai_config.json
    // directory, a .bin, …).
    backends::CheckpointResolveContext ctx;
    ctx.hf_cache = hf_cache;
    ctx.repo_id = checkpoint_to_repo_id(checkpoint);
    ctx.main_repo_id = checkpoint_to_repo_id(info.checkpoint("main"));
    ctx.variant = checkpoint_to_variant(checkpoint);
    ctx.registry_source = effective_registry_source(info);
    ctx.model_cache_path = hf_cache + "/" + repo_id_to_cache_dir_name(ctx.repo_id, ctx.registry_source);
    ctx.type = type;
    ctx.checkpoint = checkpoint;

    return backends::ops_for(info.recipe)->resolve_checkpoint_path(info, ctx);
}

void ModelManager::resolve_all_model_paths(ModelInfo& info) {
    for (auto const& [type, checkpoint] : info.checkpoints) {
        info.resolved_paths[type] = resolve_model_path(info, type, checkpoint);
    }
}

json ModelManager::load_server_models() {
    try {
        // Load from resources directory (relative to executable)
        std::string models_path = get_resource_path("resources/server_models.json");
        return JsonUtils::load_from_file(models_path);
    } catch (const std::exception& e) {
        LOG(ERROR, "ModelManager") << "Failed to load server_models.json: " << e.what() << std::endl;
        LOG(ERROR, "ModelManager") << "This is a critical file required for the application to run." << std::endl;
        LOG(ERROR, "ModelManager") << "Executable directory: " << get_executable_dir() << std::endl;
        throw std::runtime_error("Failed to load server_models.json");
    }
}

json ModelManager::load_optional_json(const std::string& path) {
    if (!fs::exists(path)) {
        return json::object();
    }

    try {
        LOG(INFO, "ModelManager") << "Loading " << fs::path(path).filename() << std::endl;
        return JsonUtils::load_from_file(path);
    } catch (const std::exception& e) {
        LOG(WARNING, "ModelManager") << "Could not load " << fs::path(path).filename() << ": " << e.what() << std::endl;
        return json::object();
    }
}

json ModelManager::load_architecture_defaults() {
    try {
        std::string path = get_resource_path("resources/architecture_defaults.json");
        return JsonUtils::load_from_file(path);
    } catch (const std::exception& e) {
        LOG(WARNING, "ModelManager") << "Could not load architecture_defaults.json: " << e.what() << std::endl;
        return json::object();
    }
}

json ModelManager::get_architecture_defaults(const std::string& architecture) const {
    if (architecture.empty() || !architecture_defaults_.is_object()) {
        return json::object();
    }
    if (architecture_defaults_.contains(architecture) &&
        architecture_defaults_[architecture].is_object()) {
        return architecture_defaults_[architecture];
    }
    return json::object();
}

static void save_user_json(const std::string& save_path, const json& to_save) {
    // Ensure directory exists
    fs::path target = path_from_utf8(save_path);
    fs::path dir = target.parent_path();
    ensure_create_directories(dir);

    LOG(INFO, "ModelManager") << "Saving " << target.filename() << std::endl;

    // Write via a unique sibling temp file and then rename into place. Readers
    // should never observe a truncated or half-written registry, and concurrent
    // writers should not collide on the same temporary path.
    std::ostringstream tmp_suffix;
    tmp_suffix << ".tmp."
               << std::this_thread::get_id() << "."
               << std::chrono::steady_clock::now().time_since_epoch().count() << "."
               << reinterpret_cast<std::uintptr_t>(&to_save);
    fs::path tmp = target;
    tmp += tmp_suffix.str();
    {
        std::ofstream file(tmp, std::ios::trunc);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open file for writing: " + path_to_utf8(tmp));
        }
        try {
            file << to_save.dump(2);
            file.flush();
        } catch (const json::exception& e) {
            throw std::runtime_error("Failed to write JSON to file " + path_to_utf8(tmp) + ": " + e.what());
        }
        if (!file) {
            throw std::runtime_error("Failed to flush JSON file: " + path_to_utf8(tmp));
        }
    }

    std::error_code ec;
    fs::rename(tmp, target, ec);
#ifdef _WIN32
    if (ec) {
        ec.clear();
        fs::remove(target, ec);
        ec.clear();
        fs::rename(tmp, target, ec);
    }
#endif
    if (ec) {
        std::error_code cleanup_ec;
        fs::remove(tmp, cleanup_ec);
        throw std::runtime_error("Failed to replace JSON file " + save_path + ": " + ec.message());
    }
}

void ModelManager::save_user_models(const json& user_models) {
    save_user_json(get_user_models_file(), user_models);
}

void ModelManager::save_model_options(const ModelInfo& info) {
    LOG(INFO, "ModelManager") << "Saving options for model: " << info.model_name << std::endl;
    // Persist under canonical ID (built-ins are keyed bare in cache but
    // recipe_options.json stores them as builtin.<name>).
    recipe_options_[cache_key_to_canonical_id(info.model_name)] = info.recipe_options.to_json();
    update_model_options_in_cache(info);
    save_user_json(get_recipe_options_file(), recipe_options_);
}

std::map<std::string, ModelInfo> ModelManager::get_supported_models() {
    // Build cache if needed (lazy initialization)
    build_cache();

    // Return copy of cache (all models, including their download status)
    std::lock_guard<std::mutex> lock(models_cache_mutex_);
    std::map<std::string, ModelInfo> public_models;
    for (const auto& [name, info] : models_cache_) {
        auto it = canonical_public_names_.find(name);
        const std::string& public_name = it != canonical_public_names_.end() ? it->second : name;
        ModelInfo public_info = info;
        public_info.model_name = public_name;
        public_models[public_name] = std::move(public_info);
    }
    return public_models;
}

std::vector<std::string> ModelManager::check_for_model_updates() {
    std::lock_guard<std::mutex> update_check_lock(update_check_mutex_);

    if (auto* cfg = RuntimeConfig::global(); cfg && cfg->offline()) {
        LOG(DEBUG, "ModelManager")
            << "Offline mode enabled, skipping model update check" << std::endl;
        return {};
    }

    struct RepoEntry {
        std::vector<std::string> model_names;
        std::string cached_snapshot;
        std::string repo_id;
        std::string registry_source;
    };

    std::unordered_map<std::string, RepoEntry> repos;

    {
        std::lock_guard<std::mutex> lock(models_cache_mutex_);

        if (!cache_valid_) {
            return {};
        }

        for (const auto& [name, info] : models_cache_) {
            if (!info.downloaded) {
                continue;
            }

            // FLM and cloud models are managed by their own backends.
            if (info.recipe == "flm" || info.recipe == "cloud") {
                continue;
            }

            // Local-path, local-upload and extra-directory models have no
            // remote registry to check.
            if (!info.source.empty()) {
                continue;
            }

            const std::string main_cp = info.checkpoint("main");
            if (main_cp.empty()) {
                continue;
            }

            const std::string repo_id = checkpoint_to_repo_id(main_cp);
            if (repo_id.empty()) {
                continue;
            }

            const std::string source = effective_registry_source(info);
            const std::string key = source + ":" + repo_id;

            auto& entry = repos[key];
            entry.repo_id = repo_id;
            entry.registry_source = source;
            entry.model_names.push_back(name);

            if (entry.cached_snapshot.empty()) {
                const fs::path cache_path =
                    path_from_utf8(get_hf_cache_dir()) /
                    repo_id_to_cache_dir_name(repo_id, source);

                entry.cached_snapshot = read_hf_ref_main(cache_path);
            }
        }
    }

    std::unordered_set<std::string> updated_models;
    std::unordered_set<std::string> verified_models;

    for (auto& [key, entry] : repos) {
        (void)key;

        // Without a local snapshot reference, there is nothing reliable to
        // compare against.
        if (entry.cached_snapshot.empty()) {
            continue;
        }

        try {
            const auto source =
                parse_remote_registry_source(entry.registry_source);
            const auto& registry = model_registry(source);

            LOG(DEBUG, "ModelManager")
                << "Checking for updates on "
                << remote_registry_display_name(source)
                << ": " << entry.repo_id << std::endl;

            const RegistryRepository latest =
                registry.fetch_repository(entry.repo_id);

            if (latest.snapshot_id.empty()) {
                continue;
            }

            // Only a successful registry response with a usable snapshot may
            // clear an update flag discovered by an earlier check.
            verified_models.insert(
                entry.model_names.begin(),
                entry.model_names.end());

            if (latest.snapshot_id == entry.cached_snapshot) {
                continue;
            }

            LOG(INFO, "ModelManager")
                << "Update available for " << entry.repo_id
                << " on " << remote_registry_display_name(source)
                << ": cached=" << entry.cached_snapshot.substr(0, 18)
                << ", latest=" << latest.snapshot_id.substr(0, 18)
                << " (" << entry.model_names.size()
                << " variant(s))" << std::endl;

            updated_models.insert(
                entry.model_names.begin(),
                entry.model_names.end());

        } catch (const RegistryNotFoundError& e) {
            LOG(DEBUG, "ModelManager")
                << e.what() << ", skipping update check" << std::endl;

        } catch (const std::exception& e) {
            LOG(WARNING, "ModelManager")
                << "Failed to check updates for "
                << entry.repo_id
                << " on " << entry.registry_source
                << ": " << e.what() << std::endl;
        }
    }

    std::vector<std::string> public_updated_models;

    {
        std::lock_guard<std::mutex> lock(models_cache_mutex_);

        for (auto& [name, info] : models_cache_) {
            if (verified_models.count(name)) {
                info.update_available =
                    updated_models.count(name) != 0;
            }
        }

        public_updated_models.reserve(updated_models.size());

        for (const auto& name : updated_models) {
            const auto public_it =
                canonical_public_names_.find(name);

            public_updated_models.push_back(
                public_it != canonical_public_names_.end()
                    ? public_it->second
                    : name);
        }
    }

    std::sort(
        public_updated_models.begin(),
        public_updated_models.end());

    if (!public_updated_models.empty()) {
        LOG(INFO, "ModelManager")
            << "Updates available for "
            << public_updated_models.size()
            << " model(s)" << std::endl;
    }

    return public_updated_models;
}

static void load_checkpoints(ModelInfo& info, json& model_json) {
    if (model_json.contains("checkpoints") && model_json["checkpoints"].is_object()) {
        for (auto& [key, value] : model_json["checkpoints"].items()) {
            info.checkpoints[key] = value.get<std::string>();
        }
    }
}

static std::string legacy_mmproj_to_checkpoint(std::string main, std::string mmproj) {
    return checkpoint_to_repo_id(main) + ":" + mmproj;
}

static void parse_legacy_mmproj(ModelInfo& info, const json& model_json) {
    std::string mmproj = JsonUtils::get_or_default<std::string>(model_json, "mmproj", "");

    if (!mmproj.empty()) {
        std::string main = JsonUtils::get_or_default<std::string>(model_json, "checkpoint", "");
        info.checkpoints["mmproj"] = legacy_mmproj_to_checkpoint(main, mmproj);
    }
}

static void parse_components(ModelInfo& info, const json& model_json) {
    if (!model_json.contains("components") || !model_json["components"].is_array()) {
        return;
    }

    for (const auto& component : model_json["components"]) {
        if (component.is_string()) {
            info.components.push_back(component.get<std::string>());
        }
    }
}

static std::string join_conflict_parts(const std::vector<std::string>& parts) {
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += "; ";
        result += parts[i];
    }
    return result;
}

static std::string requested_registry_source(const json& requested) {
    const std::string public_source = requested.value("source", std::string());
    const std::string explicit_source = requested.value("registry_source", std::string());

    std::string normalized_explicit;
    if (!explicit_source.empty()) {
        normalized_explicit = remote_registry_source_name(
            parse_remote_registry_source(explicit_source));
    }

    if (is_remote_registry_source(public_source)) {
        const std::string normalized_public = remote_registry_source_name(
            parse_remote_registry_source(public_source));
        if (!normalized_explicit.empty() && normalized_explicit != normalized_public) {
            throw std::invalid_argument(
                "Model source and registry_source identify different registries");
        }
        return normalized_public;
    }

    return normalized_explicit;
}

static std::string describe_registration_conflict(const ModelInfo& existing,
                                                  const json& requested) {
    std::vector<std::string> diffs;

    auto add_diff = [&diffs](const std::string& field,
                             const std::string& current_value,
                             const std::string& requested_value) {
        if (!requested_value.empty() && requested_value != current_value) {
            diffs.push_back(field + " (existing='" + current_value +
                            "', requested='" + requested_value + "')");
        }
    };

    std::string requested_main;
    if (requested.contains("checkpoints") && requested["checkpoints"].is_object()) {
        requested_main = requested["checkpoints"].value("main", std::string());
        for (const auto& [role, value] : requested["checkpoints"].items()) {
            if (!value.is_string()) continue;
            add_diff("checkpoint[" + role + "]",
                     existing.checkpoint(role),
                     value.get<std::string>());
        }
    } else {
        requested_main = requested.value("checkpoint", std::string());
        add_diff("checkpoint", existing.checkpoint(), requested_main);
    }

    const std::string requested_recipe = requested.value("recipe", std::string());
    add_diff("recipe", existing.recipe, requested_recipe);

    const std::string requested_source = requested_registry_source(requested);
    add_diff("source", effective_registry_source(existing), requested_source);

    const std::string requested_mmproj = requested.value("mmproj", std::string());
    if (!requested_mmproj.empty()) {
        const std::string main_for_mmproj = requested_main.empty()
            ? existing.checkpoint()
            : requested_main;
        add_diff("mmproj",
                 existing.checkpoint("mmproj"),
                 legacy_mmproj_to_checkpoint(main_for_mmproj, requested_mmproj));
    }

    return join_conflict_parts(diffs);
}

// Check if all components of a collection model are downloaded.
static bool check_component_downloaded(const ModelInfo& info,
                                        const std::map<std::string, ModelInfo>& model_map) {
    if (info.components.empty()) return false;
    for (const auto& component_name : info.components) {
        auto it = model_map.find(component_name);
        if (it == model_map.end() || !it->second.downloaded) {
            return false;
        }
    }
    return true;
}

static bool has_partial_files(const fs::path& dir) {
    std::error_code ec;
    if (!safe_is_directory(dir)) return false;
    // Non-recursive scan for .partial markers to confirm folder integrity
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".partial") {
            return true;
        }
    }
    return false;
}

static bool is_checkpoint_path_complete(const std::string& path_str) {
    if (path_str.empty()) return false;

    fs::path resolved = path_from_utf8(path_str);
    if (!safe_exists(resolved)) return false;

    // A manifest or .partial file indicates an interrupted multi-file download.
    // Preserve the existing semantics: file checkpoints check their parent
    // directory for the manifest and their own .partial marker; directory
    // checkpoints check the directory itself.
    fs::path marker_dir = safe_is_directory(resolved) ? resolved : resolved.parent_path();
    if (safe_exists(marker_dir / ".download_manifest.json")) return false;

    if (!safe_is_directory(resolved)) {
        return !safe_exists(path_from_utf8(path_str + ".partial"));
    }

    return !has_partial_files(resolved);
}

/**
 * Returns true if all files required by the model recipe are present and complete.
 * Note: npu_cache is skipped as it is managed lazily by the flm-npu backend.
 */
static bool are_required_checkpoints_complete(const ModelInfo& info) {
    for (const auto& [type, checkpoint] : info.checkpoints) {
        (void)checkpoint;

        if (type == "npu_cache") continue;

        const std::string resolved_path = info.resolved_path(type);
        if (!is_checkpoint_path_complete(resolved_path)) {
            return false;
        }

        // Per-backend file validation (e.g. llamacpp checks GGUF magic).
        std::string invalid = backends::ops_for(info.recipe)->validate_checkpoint_file(resolved_path);
        if (!invalid.empty()) {
            LOG(WARNING, "ModelManager")
                << invalid << "; marking model as not downloaded: " << resolved_path << std::endl;
            return false;
        }
    }
    return true;
}

bool ModelManager::checkpoints_complete(const ModelInfo& info) const {
    return are_required_checkpoints_complete(info);
}

void ModelManager::download_from_registry_engine(const ModelInfo& info,
                                                 DownloadProgressCallback progress_callback) {
    download_from_registry(info, progress_callback);
}

void ModelManager::download_from_huggingface_engine(
        const ModelInfo& info, DownloadProgressCallback progress_callback) {
    download_from_registry_engine(info, progress_callback);
}

void ModelManager::build_cache() {
    std::lock_guard<std::mutex> lock(models_cache_mutex_);

    if (cache_valid_) {
        return;
    }

    LOG(INFO, "ModelManager") << "Building models cache..." << std::endl;

    models_cache_.clear();
    std::map<std::string, ModelInfo> all_models;
    std::map<std::string, json> json_recipe_options;  // Per-model recipe_options from JSON

    // Step 1: Load ALL models from JSON (server models)
    for (auto& [key, value] : server_models_.items()) {
        ModelInfo info;
        info.model_name = key;
        info.checkpoints["main"] = JsonUtils::get_or_default<std::string>(value, "checkpoint", "");
        parse_legacy_mmproj(info, value);
        load_checkpoints(info, value);
        parse_components(info, value);
        info.recipe = JsonUtils::get_or_default<std::string>(value, "recipe", "");
        info.suggested = JsonUtils::get_or_default<bool>(value, "suggested", false);
        try {
            parse_model_source_fields(info, value);
        } catch (const std::exception& e) {
            LOG(ERROR, "ModelManager")
                << "Skipping invalid built-in model '" << key
                << "': " << e.what() << std::endl;
            continue;
        }
        info.size = JsonUtils::get_or_default<double>(value, "size", 0.0);
        info.cloud_provider = JsonUtils::get_or_default<std::string>(value, "cloud_provider", "");
        info.system_prompt = JsonUtils::get_or_default<std::string>(value, "system_prompt", "");

        // Registry-backed collections store their components remotely — the
        // cached manifest is the single source of truth. Rebuild the component
        // list from it on every cache build whenever a repo pointer is present,
        // so a refreshed manifest is always reflected (and any stale components
        // a previous version may have persisted are ignored/self-healed). A
        // pure inline collection has no checkpoint pointer; it keeps its
        // authored components and only falls back to the cache when empty.
        if (is_model_collection_recipe(info.recipe) &&
            (info.components.empty() || !info.checkpoint().empty())) {
            info.components.clear();
            populate_collection_components_from_cache_locked(info);
        }

        if (value.contains("labels") && value["labels"].is_array()) {
            for (const auto& label : value["labels"]) {
                info.labels.push_back(label.get<std::string>());
            }
        }

        parse_image_defaults(info, value);
        parse_extras(info, value);

        // Parse recipe_options if present (for per-model runtime config like sdcpp_args)
        if (value.contains("recipe_options") && value["recipe_options"].is_object()) {
            json_recipe_options[key] = value["recipe_options"];
        }

        // Populate type and device fields (multi-model support)
        info.type = get_model_type_from_labels(info.labels);
        info.device = device_type_for_recipe(info.recipe);

        try {
            resolve_all_model_paths(info);
        } catch (const std::exception& e) {
            LOG(ERROR, "ModelManager") << "  EXCEPTION resolving '" << key << "': " << e.what() << std::endl;
        }
        all_models[key] = info;
    }

    // Load user models with "user." prefix
    for (auto& [key, value] : user_models_.items()) {
        ModelInfo info;
        info.model_name = "user." + key;
        info.checkpoints["main"] = JsonUtils::get_or_default<std::string>(value, "checkpoint", "");
        parse_legacy_mmproj(info, value);
        load_checkpoints(info, value);
        parse_components(info, value);
        info.recipe = JsonUtils::get_or_default<std::string>(value, "recipe", "");
        info.suggested = JsonUtils::get_or_default<bool>(value, "suggested", true);
        try {
            parse_model_source_fields(info, value);
        } catch (const std::exception& e) {
            LOG(ERROR, "ModelManager")
                << "Skipping invalid user model '" << info.model_name
                << "': " << e.what() << std::endl;
            continue;
        }
        info.size = JsonUtils::get_or_default<double>(value, "size", 0.0);
        info.cloud_provider = JsonUtils::get_or_default<std::string>(value, "cloud_provider", "");
        info.system_prompt = JsonUtils::get_or_default<std::string>(value, "system_prompt", "");

        // Registry-backed user collections (created by `lemonade pull <org>/<repo>` or `--source`)
        // keep only a repo pointer in user_models.json; their components live in
        // the cached manifest. Rebuild them from it whenever a pointer is present
        // so a refreshed manifest is reflected and no stale list can shadow it.
        // A pure inline user collection has no checkpoint pointer and keeps its
        // authored components, falling back to the cache only when empty.
        if (is_model_collection_recipe(info.recipe) &&
            (info.components.empty() || !info.checkpoint().empty())) {
            info.components.clear();
            populate_collection_components_from_cache_locked(info);
        }

        if (value.contains("labels") && value["labels"].is_array()) {
            for (const auto& label : value["labels"]) {
                info.labels.push_back(label.get<std::string>());
            }
        }

        parse_image_defaults(info, value);
        parse_extras(info, value);

        // Parse recipe_options if present (for per-model runtime config like sdcpp_args)
        if (value.contains("recipe_options") && value["recipe_options"].is_object()) {
            json_recipe_options[info.model_name] = value["recipe_options"];
        }

        // Populate type and device fields (multi-model support)
        info.type = get_model_type_from_labels(info.labels);
        info.device = device_type_for_recipe(info.recipe);

        try {
            resolve_all_model_paths(info);
        } catch (const std::exception& e) {
            LOG(ERROR, "ModelManager") << "  EXCEPTION resolving '" << info.model_name << "': " << e.what() << std::endl;
        }
        all_models[info.model_name] = info;
    }

    // Step 1.5: Discover models from extra_models_dir
    // All discovered models are prefixed with "extra." so they have distinct
    // canonical IDs from any user. or builtin. records that may share a bare
    // name. Bare-name collisions are surfaced via the friendly-name layer in
    // rebuild_public_model_aliases_locked, not by dropping records here.
    auto discovered_models = discover_extra_models();
    for (const auto& [name, info] : discovered_models) {
        if (all_models.find(name) != all_models.end()) {
            LOG(INFO, "ModelManager") << "Warning: Discovered model '" << name
                      << "' conflicts with another extra.* registration; skipping." << std::endl;
            continue;
        }
        all_models[name] = info;
    }

    // Step 1.6: Dynamic discovery. Backends whose models are supplied at runtime
    // (descriptor dynamic_models = true — flm from `flm list`, cloud from each
    // provider) contribute their models via ops->discover_models(). Each carries
    // its own downloaded status. Precedence: server/user/extra models win, so we
    // emplace (don't overwrite). Failures are handled inside each backend's ops.
    {
        const bool offline = [] {
            auto* cfg = RuntimeConfig::global();
            return cfg && cfg->offline();
        }();

        backends::BackendOpsContext octx;
        octx.model_manager = this;
        octx.cloud_registry = cloud_registry_;

        for (const auto* desc : backends::all_descriptors()) {
            if (!desc->dynamic_models) {
                continue;
            }

            if (offline && desc->recipe == "cloud") {
                LOG(DEBUG, "ModelManager")
                    << "Offline mode enabled, skipping cloud model discovery"
                    << std::endl;
                continue;
            }

            for (auto& m : backends::ops_for(desc->recipe)->discover_models(octx)) {
                all_models.emplace(m.model_name, std::move(m));
            }
        }
    }

    // Populate recipe options. recipe_options.json is keyed by canonical ID
    // (user.*, extra.*, builtin.*) — built-ins are keyed bare in the cache, so
    // we translate before lookup.
    for (auto& [name, info] : all_models) {
        json jro = json_recipe_options.count(name) ? json_recipe_options[name] : json(nullptr);
        info.recipe_options = build_recipe_options(info, jro, cache_key_to_canonical_id(name), recipe_options_);
    }

    // Step 2: Filter by backend availability
    all_models = filter_models_by_backend(all_models);

    // Step 3: Check download status for all models. Dynamic-discovery backends
    // (flm, cloud) already set downloaded during discovery; everyone else asks
    // its backend ops (default = shared registry-cache completeness check).
    backends::BackendOpsContext status_ctx;
    status_ctx.model_manager = this;

    int downloaded_count = 0;
    // First pass: determine download status for non-collection models
    for (auto& [name, info] : all_models) {
        if (is_model_collection_recipe(info.recipe)) {
            continue;  // Handled in second pass after components are resolved
        }
        const auto* desc = backends::descriptor_for(info.recipe);
        if (!(desc && desc->dynamic_models)) {
            info.downloaded = backends::ops_for(info.recipe)->is_downloaded(info, status_ctx);
        }

        if (info.downloaded) {
            downloaded_count++;
        }
    }

    // Second pass: determine download status for collection models
    // (must happen after components have their downloaded status set)
    for (auto& [name, info] : all_models) {
        if (!is_model_collection_recipe(info.recipe)) continue;
        info.downloaded = check_component_downloaded(info, all_models);
        if (info.downloaded) {
            downloaded_count++;
        }
    }

    for (auto& [name, info] : all_models) {
        populate_model_metadata(info);
        if (info.downloaded) {
            refresh_on_disk_size(info);
        }
        models_cache_[name] = info;
    }

    rebuild_public_model_aliases_locked();

    cache_valid_ = true;

    // Parse each collection.router model's routing policy now, while the cache
    // and its alias map are fully built and the lock is still held. The parser's
    // component resolver needs only alias resolution, which is a direct lookup
    // into public_model_aliases_ here — exactly what resolve_model_name() does,
    // minus its own build_cache()+lock. Calling resolve_model_name() here would
    // re-lock this non-recursive mutex and deadlock, so the lookup is inlined.
    RoutingPolicyParseOptions policy_options;
    policy_options.resolve_component =
        [this](const std::string& name) -> std::optional<std::string> {
        auto it = public_model_aliases_.find(name);
        return it != public_model_aliases_.end() ? it->second : name;
    };
    for (auto& [name, info] : models_cache_) {
        if (!is_router_collection_recipe(info.recipe)) {
            continue;
        }
        json doc;
        doc["recipe"] = info.recipe;
        doc["components"] = info.components;
        auto version_it = info.extras.find("version");
        if (version_it != info.extras.end()) {
            doc["version"] = version_it->second;
        }
        auto routing_it = info.extras.find("routing");
        if (routing_it != info.extras.end()) {
            doc["routing"] = routing_it->second;
        }
        try {
            info.route_policy = std::make_shared<const RoutePolicy>(
                parse_route_policy_collection(doc, policy_options));
        } catch (const std::exception& e) {
            LOG(WARNING, "ModelManager") << "Failed to parse routing policy for '"
                                         << name << "': " << e.what() << std::endl;
        }
    }

    LOG(INFO, "ModelManager") << "Cache built: " << models_cache_.size()
              << " total, " << downloaded_count << " downloaded" << std::endl;
}

void ModelManager::add_model_to_cache(const std::string& model_name) {
    std::lock_guard<std::mutex> lock(models_cache_mutex_);

    if (!cache_valid_) {
        return; // Will initialize on next access
    }

    // Parse model name to get JSON key
    std::string json_key = model_name;
    bool is_user_model = is_user_model_name(model_name);
    if (is_user_model) {
        json_key = strip_user_model_prefix(model_name);
    }

    // Find in JSON
    json* model_json = nullptr;
    if (is_user_model && user_models_.contains(json_key)) {
        model_json = &user_models_[json_key];
    } else if (!is_user_model && server_models_.contains(json_key)) {
        model_json = &server_models_[json_key];
    }

    if (!model_json) {
        LOG(WARNING, "ModelManager") << "'" << model_name << "' not found in JSON" << std::endl;
        return;
    }

    // Build ModelInfo
    ModelInfo info;
    info.model_name = model_name;
    info.checkpoints["main"] = JsonUtils::get_or_default<std::string>(*model_json, "checkpoint", "");
    parse_legacy_mmproj(info, *model_json);
    load_checkpoints(info, *model_json);
    parse_components(info, *model_json);
    info.recipe = JsonUtils::get_or_default<std::string>(*model_json, "recipe", "");
    info.cloud_provider = JsonUtils::get_or_default<std::string>(*model_json, "cloud_provider", "");

    parse_image_defaults(info, *model_json);
    parse_extras(info, *model_json);
    json jro = (model_json->contains("recipe_options") && (*model_json)["recipe_options"].is_object())
        ? (*model_json)["recipe_options"] : json(nullptr);
    info.recipe_options = build_recipe_options(info, jro, cache_key_to_canonical_id(model_name), recipe_options_);

    info.suggested = JsonUtils::get_or_default<bool>(*model_json, "suggested", is_user_model);
    parse_model_source_fields(info, *model_json);
    info.system_prompt = JsonUtils::get_or_default<std::string>(*model_json, "system_prompt", "");

    if (model_json->contains("labels") && (*model_json)["labels"].is_array()) {
        for (const auto& label : (*model_json)["labels"]) {
            info.labels.push_back(label.get<std::string>());
        }
    }

    // Populate type and device fields (multi-model support)
    info.type = get_model_type_from_labels(info.labels);
    info.device = device_type_for_recipe(info.recipe);

    resolve_all_model_paths(info);

    // Check if it should be filtered out by backend availability
    std::map<std::string, ModelInfo> temp_map = {{model_name, info}};
    auto filtered = filter_models_by_backend(temp_map);

    if (filtered.empty()) {
        LOG(INFO, "ModelManager") << "Model '" << model_name << "' filtered out by backend availability" << std::endl;
        return; // Backend not available, don't add to cache
    }

    // Check download status (collections aggregate their components; everyone
    // else asks its backend ops).
    if (is_model_collection_recipe(info.recipe)) {
        info.downloaded = check_component_downloaded(info, models_cache_);
    } else {
        backends::BackendOpsContext octx;
        octx.model_manager = this;
        info.downloaded = backends::ops_for(info.recipe)->is_downloaded(info, octx);
    }

    populate_model_metadata(info);
    models_cache_[model_name] = info;
    rebuild_public_model_aliases_locked();
    LOG(INFO, "ModelManager") << "Added '" << model_name << "' to cache (downloaded=" << info.downloaded << ")" << std::endl;
}

void ModelManager::update_model_options_in_cache(const ModelInfo& info) {
    std::lock_guard<std::mutex> lock(models_cache_mutex_);

    if (!cache_valid_) {
        return; // Will rebuild on next access
    }

    auto it = models_cache_.find(info.model_name);
    if (it != models_cache_.end()) {
        it->second.recipe_options = info.recipe_options;
    } else {
        LOG(WARNING, "ModelManager") << "'" << info.model_name << "' not found in cache" << std::endl;
    }
}

void ModelManager::update_model_in_cache(const std::string& model_name, bool downloaded) {
    std::lock_guard<std::mutex> lock(models_cache_mutex_);

    if (!cache_valid_) {
        return; // Will rebuild on next access
    }

    auto it = models_cache_.find(model_name);
    if (it != models_cache_.end()) {
        it->second.downloaded = downloaded;

        // After a fresh download the model is up to date
        if (downloaded) {
            it->second.update_available = false;
        }

        // Recompute resolved_path after download
        // The path changes now that files exist on disk
        if (downloaded) {
            resolve_all_model_paths(it->second);
            if (backends::ops_for(it->second.recipe)->invalidates_cache_after_download()) {
                cache_valid_ = false;
                LOG(INFO, "ModelManager") << "Invalidated model cache after download for '"
                          << model_name << "' (backend rebuilds its model list)" << std::endl;
                return;
            }
            populate_model_metadata(it->second);
            LOG(INFO, "ModelManager") << "Updated '" << model_name
                      << "' downloaded=" << downloaded
                      << ", resolved_path=" << it->second.resolved_path() << std::endl;
        } else {
            it->second.max_context_window = 0;
            LOG(INFO, "ModelManager") << "Updated '" << model_name
                      << "' downloaded=" << downloaded << std::endl;
        }
        refresh_on_disk_size(it->second);

        // Recompute downloaded status for any collections that
        // depend on this model, so the collection reflects component changes
        // without requiring a full cache rebuild.
        for (auto& [name, entry] : models_cache_) {
            if (!is_model_collection_recipe(entry.recipe)) continue;
            if (std::find(entry.components.begin(), entry.components.end(),
                          model_name) == entry.components.end()) {
                continue;
            }
            bool new_state = check_component_downloaded(entry, models_cache_);
            if (entry.downloaded != new_state) {
                entry.downloaded = new_state;
                LOG(INFO, "ModelManager") << "Collection '" << name
                          << "' downloaded=" << new_state << " (dependent on " << model_name << ")" << std::endl;
            }
        }
    } else {
        LOG(WARNING, "ModelManager") << "'" << model_name << "' not found in cache" << std::endl;
    }
}

void ModelManager::remove_model_from_cache(const std::string& model_name) {
    std::lock_guard<std::mutex> lock(models_cache_mutex_);

    if (!cache_valid_) {
        return;
    }

    auto it = models_cache_.find(model_name);
    if (it != models_cache_.end()) {
        // User models and local uploads should be removed entirely from cache
        // (they're not in server_models.json, so keeping them makes no sense)
        bool is_user_model = is_user_model_name(model_name);
        if (is_user_model || it->second.source == "local_upload") {
            models_cache_.erase(model_name);
            rebuild_public_model_aliases_locked();
            LOG(INFO, "ModelManager") << "Removed '" << model_name << "' from cache" << std::endl;
        } else {
            // Registered model - just mark as not downloaded
            it->second.downloaded = false;
            it->second.update_available = false;
            LOG(INFO, "ModelManager") << "Marked '" << model_name << "' as not downloaded" << std::endl;
        }
    }
}


std::map<std::string, ModelInfo> ModelManager::get_downloaded_models() {
    // Build cache if needed
    build_cache();

    // Filter and return downloaded models. Collections (Omni) are included once
    // all their components are downloaded: the server orchestrates /chat/completions
    // for them (see CollectionOrchestrator), so they are genuine OpenAI-compatible
    // chat models and should appear in /v1/models and Ollama /api/tags. A collection's
    // `downloaded` flag already reflects component status (see build_cache).
    std::lock_guard<std::mutex> lock(models_cache_mutex_);
    std::map<std::string, ModelInfo> downloaded;
    for (const auto& [name, info] : models_cache_) {
        if (info.downloaded) {
            auto it = canonical_public_names_.find(name);
            const std::string& public_name = it != canonical_public_names_.end() ? it->second : name;
            ModelInfo public_info = info;
            public_info.model_name = public_name;
            downloaded[public_name] = std::move(public_info);
        }
    }
    return downloaded;
}

// Helper function to parse physical memory string (e.g., "32.00 GB") to GB as double
// Returns 0.0 if parsing fails
static double parse_physical_memory_gb(const std::string& memory_str) {
    if (memory_str.empty()) {
        return 0.0;
    }

    // Expected format: "XX.XX GB" or "XX GB"
    std::istringstream iss(memory_str);
    double value = 0.0;
    std::string unit;

    if (iss >> value >> unit) {
        // Convert to lowercase for comparison
        std::transform(unit.begin(), unit.end(), unit.begin(), ::tolower);
        if (unit == "gb") {
            return value;
        } else if (unit == "mb") {
            return value / 1024.0;
        } else if (unit == "tb") {
            return value * 1024.0;
        }
    }

    return 0.0;
}


double get_max_memory_of_device(json device, MemoryAllocBehavior mem_alloc_behavior) {
    // Get the maximum POSSIBLE accessible memory of the device in question,
    // taking into account the respective memory allocation behavior.

    double virtual_mem_gb = 0.0;
    double vram_gb = 0.0;

    if (device.contains("vram_gb")) {
        vram_gb = device["vram_gb"].get<double>();
    }
    if (device.contains("virtual_mem_gb"))
    {
        virtual_mem_gb = device["virtual_mem_gb"].get<double>();
    }

    switch (mem_alloc_behavior)
    {
    case MemoryAllocBehavior::Hardware:
        return vram_gb;

    case MemoryAllocBehavior::Virtual:
        return virtual_mem_gb;

    case MemoryAllocBehavior::Largest:
        return vram_gb > virtual_mem_gb ? vram_gb : virtual_mem_gb;

    case MemoryAllocBehavior::Unified:
        return virtual_mem_gb + vram_gb;

    default:
        return vram_gb;
    }
}

bool parse_TF_env_var(const char* env_var_name) {
    const char* env = std::getenv(env_var_name);
    return env && (std::string(env) == "1" ||
                   std::string(env) == "true" ||
                   std::string(env) == "TRUE" ||
                   std::string(env) == "yes");
}

std::map<std::string, ModelInfo> ModelManager::filter_models_by_backend(
    const std::map<std::string, ModelInfo>& models) {

    // Check if model filtering is disabled via config.json
    bool disable_filtering = false;
    bool enable_dgpu_gtt = false;
    auto* cfg = lemon::RuntimeConfig::global();
    if (cfg) {
        disable_filtering = cfg->disable_model_filtering();
        enable_dgpu_gtt = cfg->enable_dgpu_gtt();
    }

    if (disable_filtering) {
        filtered_out_models_.clear();
        return models;
    }

    if (enable_dgpu_gtt)
    {
      LOG(INFO, "ModelManager") << "enable_dgpu_gtt has been set to true." << std::endl
                << "     Models are being filtered assuming GTT memory." << std::endl
                << "     Using GTT on a dGPU will have a significant performance impact." << std::endl;
    }

    std::map<std::string, ModelInfo> filtered;

    filtered_out_models_.clear();

    json system_info = SystemInfoCache::get_system_info_with_cache();
    json hardware = system_info.contains("devices") ? system_info["devices"] : json::object();

    bool npu_available = hardware.contains("amd_npu") &&
                         hardware["amd_npu"].is_object() &&
                         hardware["amd_npu"].value("available", false);

    double largest_mem_pool_gb = 0.0;
    double curr_mem_pool_gb = 0.0;

    for (const auto& [dev_type, devices] : hardware.items()) {
        // Because we have mixed types this just makes every device_type an array.
        nlohmann::json dev_list = devices.is_array() ? devices : nlohmann::json{devices};

        // Expand this later to accommodate mixed pools
        MemoryAllocBehavior dev_mem_alloc_behavior = MemoryAllocBehavior::Hardware;
        if (dev_type == "amd_igpu")
            dev_mem_alloc_behavior = MemoryAllocBehavior::Largest;
        if (enable_dgpu_gtt)
            dev_mem_alloc_behavior = MemoryAllocBehavior::Unified;

        for (const auto& dev : dev_list) {
            curr_mem_pool_gb = get_max_memory_of_device(dev, dev_mem_alloc_behavior);
            largest_mem_pool_gb = largest_mem_pool_gb < curr_mem_pool_gb ? curr_mem_pool_gb : largest_mem_pool_gb;
        }
    }

    double system_ram_gb = 0.0;
    if (system_info.contains("Physical Memory") && system_info["Physical Memory"].is_string()) {
        system_ram_gb = parse_physical_memory_gb(system_info["Physical Memory"].get<std::string>());
    }

    double max_model_size_gb = largest_mem_pool_gb > (system_ram_gb * 0.8) ? largest_mem_pool_gb : (system_ram_gb * 0.8);

    std::string processor = "Unknown";
    std::string os_version = "Unknown";
    if (system_info.contains("Processor") && system_info["Processor"].is_string()) {
        processor = system_info["Processor"].get<std::string>();
    }
    if (system_info.contains("OS Version") && system_info["OS Version"].is_string()) {
        os_version = system_info["OS Version"].get<std::string>();
    }

    static bool debug_printed = false;
    if (!debug_printed) {
        LOG(INFO, "ModelManager") << "Backend availability:" << std::endl;
        LOG(INFO, "ModelManager") << "  - NPU hardware: " << (npu_available ? "Yes" : "No") << std::endl;
        if (system_ram_gb > 0.0) {
            LOG(INFO, "ModelManager") << "  - System RAM: " << std::fixed << std::setprecision(1) << system_ram_gb
                      << " GB (max model size: " << max_model_size_gb << " GB)" << std::endl;
        }
        if (largest_mem_pool_gb > 0.0) {
            LOG(INFO, "ModelManager") << "  - Largest memory pool: " << std::fixed << std::setprecision(1) << largest_mem_pool_gb << std::endl;
        }
        if (system_info.contains("devices") && system_info["devices"].contains("nvidia_gpu")) {
            const auto& nvidia_gpus = system_info["devices"]["nvidia_gpu"];
            if (nvidia_gpus.is_array()) {
                for (const auto& gpu : nvidia_gpus) {
                    if (gpu.value("available", false)) {
                        std::string name = gpu.value("name", "unknown");
                        std::string family = gpu.value("family", "");
                        std::string cc = gpu.value("compute_capability", "");
                        std::string suffix;
                        if (!cc.empty()) suffix = " (compute " + cc + ", " + (family.empty() ? "unsupported arch" : family) + ")";
                        else if (!family.empty()) suffix = " (" + family + ")";
                        else suffix = " (arch unknown -- nvidia-smi may have failed)";
                        LOG(INFO, "ModelManager") << "  - NVIDIA GPU: " << name << suffix << std::endl;
                    } else if (gpu.contains("error")) {
                        LOG(INFO, "ModelManager") << "  - NVIDIA GPU: detection error: " << gpu["error"].get<std::string>() << std::endl;
                    }
                }
                if (system_info["devices"].contains("nvidia_gpu_error")) {
                    LOG(INFO, "ModelManager") << "  - NVIDIA GPU: detection error: "
                        << system_info["devices"]["nvidia_gpu_error"].get<std::string>() << std::endl;
                }
            }
        }
        debug_printed = true;
    }

    for (const auto& [name, info] : models) {
        const std::string& recipe = info.recipe;
        bool filter_out = false;
        std::string filter_reason;

        // Collections are UI-level entries that orchestrate components.
        // They should always be visible if present in the registry.
        if (is_model_collection_recipe(recipe)) {
            filtered[name] = info;
            continue;
        }

        // Cloud-offloaded models bypass local backend/RAM checks (the model
        // executes on a remote provider). Discovery is server-side and runs
        // at every cache build, so this branch normally sees the full set
        // of discovered cloud entries plus anything a user pinned into
        // user_models.json.
        if (recipe == "cloud") {
            filtered[name] = info;
            continue;
        }

        const bool user_controlled_model = is_user_model_name(name) ||
                                           is_extra_model_name(name) ||
                                           info.source == "local_upload";

        // Check recipe support using the centralized system_info recipes structure
        std::string unsupported_reason = SystemInfo::check_recipe_supported(recipe);
        if (!unsupported_reason.empty()) {
            filter_out = true;
            filter_reason = unsupported_reason + " "
                           "Detected processor: " + processor + ". "
                           "Detected operating system: " + os_version + ".";
        }

        // Filter out models that are too large for system RAM
        // Heuristic: if model size > 80% of system RAM, filter it out
        if (!filter_out && !user_controlled_model && system_ram_gb > 0.0 && info.size > 0.0) {
            if (info.size > max_model_size_gb) {
                filter_out = true;
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(1);
                oss << "This model requires approximately " << info.size << " GB of memory, "
                    << "but your system only has " << system_ram_gb << " GB of RAM. "
                    << "Models larger than " << max_model_size_gb << " GB (80% of system RAM) are filtered out.";
                filter_reason = oss.str();
            }
        }

        // Special rule: filter out gpt-oss-20b-FLM on Windows systems with less than 48 GB RAM
#ifdef _WIN32
        if (!filter_out && name == "gpt-oss-20b-FLM" && system_ram_gb > 0.0 && system_ram_gb < 48.0) {
            filter_out = true;
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1);
            oss << "The gpt-oss-20b-FLM model requires at least 48 GB of RAM. "
                << "Your system has " << system_ram_gb << " GB.";
            filter_reason = oss.str();
        }
#endif

        if (filter_out) {
            // Store the filter reason for later lookup
            filtered_out_models_[name] = filter_reason;
            continue;
        }

        // Model passes all filters
        filtered[name] = info;
    }

    return filtered;
}

void ModelManager::set_cloud_registry(CloudProviderRegistry* registry) {
    cloud_registry_ = registry;
}

size_t ModelManager::refresh_cloud_models(const std::string& provider) {
    if (provider.empty() || cloud_registry_ == nullptr) {
        return 0;
    }

    // Resolve creds outside the cache lock — resolve_key takes a shared
    // lock on the registry's mutex internally; holding the cache lock here
    // doesn't matter but keeping it tight is cheaper.
    const std::string api_key = cloud_registry_->resolve_key(provider);
    const std::string base_url = cloud_registry_->base_url_for(provider);
    if (api_key.empty() || base_url.empty()) {
        // Drop any stale entries for this provider but don't try to discover —
        // there's nothing to discover with. The contract is "models present
        // after refresh", so return 0 (not the evicted count).
        evict_cloud_models(provider);
        return 0;
    }
    if (CloudProviderRegistry::is_http_base_url(base_url) &&
        !cloud_registry_->allow_insecure_http_for(provider)) {
        LOG(WARNING, "ModelManager") << "Skipping cloud discovery for provider '"
                                      << provider << "': http:// with API key "
                                      << "requires allow_insecure_http=true"
                                      << std::endl;
        evict_cloud_models(provider);
        return 0;
    }

    // discover_models() is best-effort; it logs upstream failures and
    // returns an empty list rather than throwing, so we can keep going for
    // other providers regardless of network state. This call happens
    // outside the cache lock because it can take up to 15 s on a slow
    // provider and we don't want to block /models on it.
    std::vector<ModelInfo> models;
    try {
        models = backends::CloudServer::discover_models(
            provider, api_key, base_url,
            cloud_registry_->allow_insecure_http_for(provider));
    } catch (const std::exception& e) {
        LOG(WARNING, "ModelManager") << "Cloud discovery threw for provider '"
                                      << provider << "': " << e.what() << std::endl;
        // Same contract: evict stale entries, report 0 present after refresh.
        evict_cloud_models(provider);
        return 0;
    }

    std::lock_guard<std::mutex> lock(models_cache_mutex_);

    // Reseed: drop this provider's previously-registered entries before
    // inserting the fresh list, so a model the provider stopped exposing
    // disappears. Other providers' entries are untouched.
    for (auto it = models_cache_.begin(); it != models_cache_.end();) {
        if (it->second.recipe == "cloud" && it->second.cloud_provider == provider) {
            it = models_cache_.erase(it);
        } else {
            ++it;
        }
    }

    size_t added = 0;
    for (const auto& m : models) {
        if (m.recipe != "cloud" || m.model_name.empty()) continue;
        // Match build_cache()'s precedence exactly: emplace, don't overwrite.
        // Any pre-existing entry under the same bare cache key wins — whether
        // it's an FLM model, another cloud provider that discovered first, or
        // a builtin/extra/user record. This provider's previously-registered
        // entries are already cleared above, so emplace here is symmetric
        // with build_cache and immune to a fast /cloud/auth racing past an
        // already-populated cache.
        ModelInfo info = m;
        // discover_models() populates name/checkpoint/labels/context/cost but
        // not recipe_options; Router needs it to construct CloudServer.
        info.recipe_options = RecipeOptions("cloud", json::object());
        auto [it, inserted] = models_cache_.emplace(info.model_name, std::move(info));
        if (!inserted) {
            LOG(INFO, "ModelManager")
                << "Cloud discovery for '" << provider << "' skipping '"
                << it->first << "': name already held (recipe="
                << it->second.recipe << ")" << std::endl;
            continue;
        }
        ++added;
    }

    rebuild_public_model_aliases_locked();

    LOG(INFO, "ModelManager") << "Refreshed cloud models for provider '"
                               << provider << "': " << added << " model(s)"
                               << std::endl;
    return added;
}

size_t ModelManager::evict_cloud_models(const std::string& provider) {
    if (provider.empty()) return 0;
    std::lock_guard<std::mutex> lock(models_cache_mutex_);
    size_t removed = 0;
    for (auto it = models_cache_.begin(); it != models_cache_.end();) {
        if (it->second.recipe == "cloud" && it->second.cloud_provider == provider) {
            it = models_cache_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    if (removed > 0) {
        rebuild_public_model_aliases_locked();
        LOG(DEBUG, "ModelManager") << "Evicted " << removed
                                    << " cloud model(s) for provider '"
                                    << provider << "'" << std::endl;
    }
    return removed;
}

size_t ModelManager::count_cloud_models(const std::string& provider) const {
    std::lock_guard<std::mutex> lock(models_cache_mutex_);
    return std::count_if(models_cache_.begin(), models_cache_.end(),
                         [&](const auto& kv) {
                             return kv.second.recipe == "cloud" &&
                                    kv.second.cloud_provider == provider;
                         });
}

void ModelManager::register_user_model(const std::string& model_name,
                                      const json& model_data,
                                      const std::string& source) {
    // Remove "user." prefix if present
    std::string clean_name = model_name;
    if (is_user_model_name(clean_name)) {
        clean_name = strip_user_model_prefix(clean_name);
    }

    // Filter only known, user-definable model props
    json model_entry;
    for (const std::string& prop : USER_DEFINED_MODEL_PROPS) {
        if (model_data.contains(prop)) {
            model_entry[prop] = model_data[prop];
        }
    }
    std::set<std::string> labels = {"custom"};
    std::vector<std::string> extra_labels = model_data.value("labels", std::vector<std::string>{});
    labels.insert(extra_labels.begin(), extra_labels.end());

    // legacy label format
    if (model_data.value("reasoning", false)) {
        labels.insert("reasoning");
    }
    if (model_data.value("vision", false)) {
        labels.insert("vision");
    }
    if (model_data.value("embedding", false)) {
        labels.insert("embeddings");
    }
    if (model_data.value("reranking", false)) {
        labels.insert("reranking");
    }

    // `recipe` already copied into `model_entry` by the USER_DEFINED_MODEL_PROPS
    // loop above; this local is just for the label inference below.
    std::string recipe = model_data.value("recipe", "");

    // Inject the backend's default labels for models that omit them (e.g. sd-cpp
    // -> image, whispercpp/moonshine -> transcription).
    if (const auto* desc = lemon::backends::descriptor_for(recipe)) {
        for (const auto& label : desc->default_labels) {
            labels.insert(label);
        }
    }

    model_entry["labels"] = labels;
    model_entry["suggested"] = true; // Always set suggested=true for user models

    if (!source.empty()) {
        model_entry["source"] = source;
    }

    // Single source of truth for registry-backed collections: the component list lives
    // in the cached remote-registry manifest, so the registry entry stores only the
    // repo pointer (checkpoint). Persisting `components` here would let a stale
    // local copy shadow a refreshed manifest. A pure inline collection has no
    // checkpoint pointer and keeps its authored components.
    if (is_model_collection_recipe(recipe)) {
        std::string pointer = model_entry.value("checkpoint", std::string());
        if (pointer.empty() && model_entry.contains("checkpoints") &&
            model_entry["checkpoints"].is_object()) {
            pointer = model_entry["checkpoints"].value("main", std::string());
        }
        if (!pointer.empty()) {
            model_entry.erase("components");
        }
    }

    // Keep the read/modify/write of user_models.json atomic. Concurrent pulls
    // can otherwise both start from the same registry snapshot and the later
    // save can drop the first model, producing a hard "Model not found" on the
    // next auto-load. Read the latest disk copy under the same process mutex so
    // stale in-memory state cannot overwrite another registration.
    {
        std::lock_guard<std::mutex> lock(models_cache_mutex_);
        json updated_user_models = load_optional_json(get_user_models_file());
        if (!updated_user_models.is_object()) {
            updated_user_models = json::object();
        }
        updated_user_models[clean_name] = model_entry;
        save_user_models(updated_user_models);
        user_models_ = std::move(updated_user_models);
        cache_valid_ = false;
    }
}

void ModelManager::unregister_user_model(const std::string& model_name) {
    std::string clean_name = model_name;
    if (is_user_model_name(clean_name)) {
        clean_name = strip_user_model_prefix(clean_name);
    }

    std::lock_guard<std::mutex> lock(models_cache_mutex_);
    json updated_user_models = load_optional_json(get_user_models_file());
    if (!updated_user_models.is_object() || !updated_user_models.contains(clean_name)) {
        return;
    }
    updated_user_models.erase(clean_name);
    save_user_models(updated_user_models);
    user_models_ = std::move(updated_user_models);
    cache_valid_ = false;
}




bool ModelManager::is_model_downloaded(const std::string& model_name) {
    // Build cache if needed
    build_cache();
    // O(1) lookup - download status is in cache
    std::lock_guard<std::mutex> lock(models_cache_mutex_);
    auto alias_it = public_model_aliases_.find(model_name);
    const std::string canonical_name = alias_it != public_model_aliases_.end()
        ? alias_it->second
        : model_name;
    auto it = models_cache_.find(canonical_name);
    if (it != models_cache_.end()) {
        if (it->second.downloaded) {
            bool still_complete = are_required_checkpoints_complete(it->second);
            if (!still_complete) {
                it->second.downloaded = false;
            }
        }
        return it->second.downloaded;
    }
    return false;
}

bool ModelManager::backend_self_manages_downloads(const std::string& recipe) const {
    const auto* desc = backends::descriptor_for(recipe);
    return desc && desc->self_manages_downloads;
}

void ModelManager::download_registered_model(const ModelInfo& info, bool do_not_upgrade, DownloadProgressCallback progress_callback) {
    // Serialize downloads per checkpoint repo. A second request for the same
    // repo (e.g. a client that timed out and retried /pull while the first
    // download is still running) must wait for the in-flight download instead
    // of writing the same .partial files concurrently, which corrupts them and
    // sends the hash verification into an endless retry-from-scratch loop.
    std::shared_ptr<std::mutex> repo_lock;
    {
        std::lock_guard<std::mutex> guard(download_locks_mutex_);
        auto& slot = download_locks_[effective_registry_source(info) + ":" + info.checkpoint()];
        if (!slot) slot = std::make_shared<std::mutex>();
        repo_lock = slot;
    }
    std::lock_guard<std::mutex> download_lock(*repo_lock);

    // The backend's ops own the download (shared registry engine by default; flm pulls
    // via the flm CLI; cloud is a no-op).
    backends::BackendOpsContext octx;
    octx.model_manager = this;
    backends::ops_for(info.recipe)->download_model(info, do_not_upgrade, progress_callback, octx);

    // Update cache after successful download
    update_model_in_cache(info.model_name, true);

    std::string canonical_model_name = resolve_model_name(info.model_name);
    if (is_user_model_name(canonical_model_name))
    {
        std::lock_guard<std::mutex> lock(models_cache_mutex_);
        auto it = models_cache_.find(info.model_name);
        if (it != models_cache_.end())
        {
            json updated_user_models = load_optional_json(get_user_models_file());
            if (!updated_user_models.is_object()) {
                updated_user_models = json::object();
            }
            auto model = updated_user_models.find(strip_user_model_prefix(canonical_model_name));
            if (model != updated_user_models.end()) {
                (*model)["size"] = it->second.size;
                save_user_models(updated_user_models);
                user_models_ = std::move(updated_user_models);
                cache_valid_ = false;
            }
        }
    }
}

// Build a ModelInfo from a raw model definition (server_models.json /
// collection-manifest shape) using the same parsing as build_cache, so a
// manifest component can be compared field-for-field against a registered model.
static ModelInfo model_info_from_def(const json& def_in) {
    json def = def_in;  // load_checkpoints needs a non-const json
    ModelInfo info;
    info.checkpoints["main"] = JsonUtils::get_or_default<std::string>(def, "checkpoint", "");
    parse_legacy_mmproj(info, def);
    load_checkpoints(info, def);
    info.recipe = JsonUtils::get_or_default<std::string>(def, "recipe", "");
    parse_model_source_fields(info, def);
    return info;
}

// Return a human-readable description of load-bearing differences between a
// registered model and a manifest's inline definition of the same component
// (empty string when they agree). Reuses model_info_from_def so identical
// definitions never produce false-positive drift.
static std::string collection_component_drift(const ModelInfo& local, const json& manifest_comp) {
    ModelInfo m = model_info_from_def(manifest_comp);
    std::vector<std::string> diffs;
    if (!m.recipe.empty() && m.recipe != local.recipe) {
        diffs.push_back("recipe (manifest='" + m.recipe + "' local='" + local.recipe + "')");
    }
    if (effective_registry_source(m) != effective_registry_source(local)) {
        diffs.push_back("source (manifest='" + effective_registry_source(m) +
                        "' local='" + effective_registry_source(local) + "')");
    }
    for (const auto& [type, ck] : m.checkpoints) {
        if (ck.empty()) continue;
        auto it = local.checkpoints.find(type);
        std::string lck = (it == local.checkpoints.end()) ? "" : it->second;
        if (ck != lck) {
            diffs.push_back("checkpoint[" + type + "] (manifest='" + ck + "' local='" + lck + "')");
        }
    }
    std::string out;
    for (size_t i = 0; i < diffs.size(); ++i) {
        if (i) out += "; ";
        out += diffs[i];
    }
    return out;
}

// Locate a collection manifest in a cached registry snapshot: any *.json file whose
// content is an object with recipe == "collection.omni". Note the two halves use
// different discovery rules by design: the *initial* remote fetch is
// filename-keyed on <RepoName>.json (see fetch_pull_variants in hf_variants.cpp),
// but once the snapshot is cached this reader is content-based, so the filename is
// not load-bearing here. Returns an empty json when no manifest is cached.
static json read_cached_collection_manifest(const fs::path& cache_dir) {
    fs::path snap = active_hf_snapshot_path(cache_dir);
    if (snap.empty()) return json();

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(snap, safe_dir_options, ec)) {
        std::error_code file_ec;
        if (!entry.is_regular_file(file_ec)) continue;
        if (entry.path().extension() != ".json") continue;
        json candidate;
        try {
            candidate = JsonUtils::load_from_file(path_to_utf8(entry.path()));
        } catch (const std::exception&) {
            continue;
        }
        if (candidate.is_object() &&
            is_model_collection_recipe(JsonUtils::get_or_default<std::string>(candidate, "recipe", ""))) {
            return candidate;
        }
    }
    return json();
}

// Bare component name: collection files reference components by public name;
// definitions may carry a `user.` prefix from the export transform.
static std::string bare_component_name(const std::string& name) {
    return is_user_model_name(name) ? strip_user_model_prefix(name) : name;
}

// A collection component's inline definition must be a usable model
// registration before we register it: a non-empty recipe plus at least one
// non-empty checkpoint. Fails an inline import closed instead of persisting a
// half-defined component that only blows up later during download.
static bool collection_component_def_is_valid(const json& def) {
    if (!def.is_object() || !def.contains("recipe") || !def["recipe"].is_string()) {
        return false;
    }
    if (def["recipe"].get<std::string>().empty()) {
        return false;
    }
    if (def.contains("checkpoint") && def["checkpoint"].is_string() &&
        !def["checkpoint"].get<std::string>().empty()) {
        return true;
    }
    if (def.contains("checkpoints") && def["checkpoints"].is_object()) {
        for (const auto& [_, value] : def["checkpoints"].items()) {
            if (value.is_string() && !value.get<std::string>().empty()) {
                return true;
            }
        }
    }
    return false;
}

json ModelManager::fetch_collection_manifest(const std::string& repo_id,
                                             const std::string& registry_source,
                                             bool do_not_upgrade) {
    const std::string source = remote_registry_source_name(parse_remote_registry_source(registry_source));
    fs::path cache_dir = path_from_utf8(get_hf_cache_dir()) / repo_id_to_cache_dir_name(repo_id, source);

    // A usable manifest needs both arrays; an incomplete cached copy (e.g. a
    // stale old-format file) must not satisfy do_not_upgrade — it should
    // trigger a refresh instead.
    auto manifest_complete = [](const json& m) {
        return m.is_object() &&
               m.contains("components") && m["components"].is_array() &&
               !m["components"].empty() &&
               m.contains("models") && m["models"].is_array();
    };

    json manifest = read_cached_collection_manifest(cache_dir);
    bool have_cache = manifest_complete(manifest);

    bool offline = false;
    if (auto* cfg = RuntimeConfig::global()) {
        offline = cfg->offline();
    }

    if (offline) {
        if (!have_cache) {
            throw std::runtime_error(
                "Offline mode: collection definition for '" + repo_id +
                "' is not cached locally.");
        }
    } else if (!(do_not_upgrade && have_cache)) {
        // Download the collection repo (the manifest plus its other small files)
        // into the shared registry cache. A bare repo id with no variant downloads all files.
        ModelInfo manifest_info;
        manifest_info.model_name = repo_id;
        manifest_info.checkpoints["main"] = repo_id;
        try {
            manifest_info.registry_source = source;
            download_from_registry(manifest_info, nullptr);
            manifest = read_cached_collection_manifest(cache_dir);
        } catch (const std::exception& e) {
            if (!have_cache) throw;
            LOG(WARNING, "ModelManager") << "Could not refresh collection manifest for "
                << repo_id << " (" << e.what() << "); using cached copy" << std::endl;
        }
    }

    if (!manifest_complete(manifest)) {
        throw std::runtime_error(
            "Collection manifest for '" + repo_id +
            "' must contain a non-empty 'components' array and a 'models' array.");
    }
    return manifest;
}

std::vector<std::string> ModelManager::register_components(const json& component_names,
                                                           const json& component_defs,
                                                           const std::string& registry_source) {
    auto def_name = [](const json& def) -> std::string {
        if (!def.is_object()) return "";
        for (const char* key : {"model_name", "id"}) {
            if (def.contains(key) && def[key].is_string()) {
                return bare_component_name(def[key].get<std::string>());
            }
        }
        return "";
    };

    // Two passes so register_components is atomic: validate every unknown
    // component's inline definition first, and only register them once the whole
    // list is known good. A half-defined component therefore never persists a
    // stray user.* entry that a later failure would have to roll back.
    std::vector<std::string> components;
    std::vector<std::pair<std::string, json>> pending_registrations;  // canonical, def
    for (const auto& entry : component_names) {
        if (!entry.is_string()) continue;
        std::string name = bare_component_name(entry.get<std::string>());
        if (name.empty()) {
            LOG(WARNING, "ModelManager") << "Skipping collection component with an "
                << "empty name" << std::endl;
            continue;
        }

        // Find the inline definition matching this component, if any.
        json def;
        if (component_defs.is_array()) {
            for (const auto& candidate : component_defs) {
                if (def_name(candidate) == name) {
                    def = candidate;
                    break;
                }
            }
        }

        // A remote collection defines one provenance domain. Components without
        // an explicit source inherit it before drift comparison or registration.
        if (def.is_object() && !def.contains("source") && !def.contains("registry_source")) {
            def["source"] = remote_registry_source_name(
                parse_remote_registry_source(registry_source));
        }

        // Components must be regular models, not collections (backstop for the
        // remote-manifest path, which does not go through validate_collection_request).
        bool def_is_collection =
            def.is_object() && is_model_collection_recipe(def.value("recipe", std::string()));
        if (def_is_collection ||
            (model_exists(name) && is_model_collection_recipe(get_model_info(name).recipe))) {
            throw std::runtime_error(
                "Collection components must be regular models, not collections: '" + name + "'");
        }

        if (model_exists(name)) {
            // Local-wins: the shipped/registered definition is authoritative. Warn
            // (do not overwrite) when the inline definition has drifted.
            if (def.is_object()) {
                ModelInfo local = get_model_info(name);
                std::string drift = collection_component_drift(local, def);
                if (!drift.empty()) {
                    LOG(WARNING, "ModelManager") << "Collection component '" << name
                        << "' differs from the local model definition; using the local "
                        << "definition. Drift: " << drift << std::endl;
                }
            }
            components.push_back(resolve_model_name(name));
        } else if (def.is_object()) {
            // Unknown component: register it from the inline definition so the
            // collection file is self-contained. Persists to user_models.json.
            std::string canonical = "user." + name;
            // Manifest content is remote input — apply the same reserved-name
            // check as the CLI and /pull registration paths.
            if (is_reserved_registration_name(canonical)) {
                LOG(WARNING, "ModelManager") << "Skipping collection component with "
                    << "reserved name: " << name << std::endl;
                continue;
            }
            // Fail closed on a half-defined component (missing recipe/checkpoint)
            // rather than registering it and failing later mid-download. The
            // throw aborts before any registration in the second pass runs.
            if (!collection_component_def_is_valid(def)) {
                throw std::runtime_error(
                    "Collection component '" + name + "' has an incomplete inline "
                    "definition (a recipe and at least one checkpoint are required).");
            }
            pending_registrations.emplace_back(canonical, def);
            components.push_back(canonical);
        } else {
            LOG(WARNING, "ModelManager") << "Skipping unknown collection component with no "
                << "inline definition: " << name << std::endl;
        }
    }

    for (const auto& [canonical, def] : pending_registrations) {
        LOG(INFO, "ModelManager") << "Registering collection component as user model: "
            << canonical << std::endl;
        register_user_model(canonical, def);
    }
    return components;
}

std::vector<std::string> ModelManager::resolve_collection_components_from_manifest(
        const std::string& repo_id, const std::string& registry_source,
        bool do_not_upgrade) {
    json manifest = fetch_collection_manifest(repo_id, registry_source, do_not_upgrade);
    return register_components(manifest["components"], manifest["models"], registry_source);
}

void ModelManager::populate_collection_components_from_cache_locked(ModelInfo& info) {
    std::string repo_id = info.checkpoint();
    if (repo_id.empty()) return;

    fs::path cache_dir = path_from_utf8(get_hf_cache_dir()) /
        repo_id_to_cache_dir_name(repo_id, effective_registry_source(info));
    json manifest = read_cached_collection_manifest(cache_dir);
    if (!manifest.is_object() || !manifest.contains("components") ||
        !manifest["components"].is_array()) {
        return;
    }

    for (const auto& entry : manifest["components"]) {
        if (!entry.is_string()) continue;
        std::string name = bare_component_name(entry.get<std::string>());
        // Compute the canonical cache name without registering or taking a lock
        // (build_cache already holds models_cache_mutex_). A built-in is keyed bare;
        // a component registered at pull time lives under user.<name>.
        std::string canonical;
        if (server_models_.contains(name)) {
            canonical = name;
        } else if (user_models_.contains(name)) {
            canonical = "user." + name;
        } else {
            canonical = name;  // not yet registered → resolves as not-downloaded
        }
        info.components.push_back(canonical);
    }

    // Surface the collection's total download size when the slim registry entry
    // omits it.
    if (info.size == 0.0 && manifest.contains("size") && manifest["size"].is_number()) {
        info.size = manifest["size"].get<double>();
    }

    // Same lift-from-manifest pattern for the optional per-collection system
    // prompt: the registry entry wins when it sets one, otherwise the published
    // remote manifest acts as the source of truth.
    if (info.system_prompt.empty() && manifest.contains("system_prompt") &&
        manifest["system_prompt"].is_string()) {
        info.system_prompt = manifest["system_prompt"].get<std::string>();
    }
}

void ModelManager::download_model(const std::string& model_name,
                                 const json& model_data,
                                 bool do_not_upgrade,
                                 DownloadProgressCallback progress_callback) {
    std::set<std::string> visited;
    download_model(model_name, model_data, do_not_upgrade, progress_callback, visited);
}

void ModelManager::download_model(const std::string& model_name,
                                 const json& model_data,
                                 bool do_not_upgrade,
                                 DownloadProgressCallback progress_callback,
                                 std::set<std::string>& visited) {
    // Keep a mutable registration payload so legacy re-pulls that omit the
    // registry retain the source recorded on the existing model. The original
    // request remains untouched for validation and download semantics.
    json registration_data = model_data;
    std::string actual_checkpoint;

    if (model_data.contains("checkpoints")) {
        json checkpoints = model_data["checkpoints"];
        if (!checkpoints.is_object() || !checkpoints.contains("main")) {
            throw std::runtime_error("If present, the `checkpoints` property must be an object and must contain `main`");
        }

        actual_checkpoint = checkpoints.value("main", "");
    } else {
        actual_checkpoint = model_data.value("checkpoint", "");
    }

    std::string actual_recipe = model_data.value("recipe", "");

    // If checkpoint or recipe are provided, this is a model registration
    // and the model name must have the "user." prefix
    if (!actual_checkpoint.empty() || !actual_recipe.empty()) {
        if (!is_user_model_name(model_name)) {
            throw std::runtime_error(
                "When providing 'checkpoint' or 'recipe', the model name must include the "
                "`user.` prefix, for example `user.Phi-4-Mini-GGUF`. Received: " +
                model_name
            );
        }
    }

    // Check if model exists in registry
    bool model_registered = model_exists(model_name);

    if (!model_registered) {
        // First, check if the model exists but was filtered out (unsupported recipe)
        if (model_exists_unfiltered(model_name)) {
            // Model exists in registry but is not available on this system
            std::string filter_reason = get_model_filter_reason(model_name);
            throw std::runtime_error(
                "Model '" + model_name + "' is not available on this system. " +
                filter_reason
            );
        }

        // Model not in registry - this must be a user model registration
        // Validate it has the "user." prefix
        if (!is_user_model_name(model_name)) {
            // See UnknownModelError contract in include/lemon/model_manager.h.
            throw UnknownModelError(
                "When registering a new model, the model name must include the "
                "`user` namespace, for example `user.Phi-4-Mini-GGUF`. Received: " +
                model_name
            );
        }

        if (is_model_collection_recipe(actual_recipe)) {
            if (auto err = validate_collection_request(model_name, model_data)) {
                throw std::runtime_error(*err);
            }
            LOG(INFO, "ModelManager") << "Registering new collection: " << model_name << std::endl;
        } else {
            // Check that required arguments are provided
            if (actual_checkpoint.empty() || actual_recipe.empty()) {
                throw std::runtime_error(
                    "Model " + model_name + " is not registered with Lemonade Server. "
                    "To register and install it, provide the `checkpoint` and `recipe` "
                    "arguments."
                );
            }

            // Backend-specific checkpoint validation (llamacpp: GGUF needs :variant).
            if (auto err = backends::ops_for(actual_recipe)->validate_registration_checkpoint(
                    actual_checkpoint);
                !err.empty()) {
                throw std::runtime_error(err);
            }

            LOG(INFO, "ModelManager") << "Registering new user model: " << model_name << std::endl;
        }
    } else {
        // Model is registered - if checkpoint not provided, look up from registry.
        // If checkpoint/recipe are provided, allow an idempotent re-pull of the
        // same registration but never silently replace a user model with a
        // different remote checkpoint or registry. Silent replacement is what
        // made a previously installed GGUF variant disappear when another variant
        // reused the same generated user.* name.
        auto info = get_model_info(model_name);
        if (!registration_data.contains("source") &&
            !registration_data.contains("registry_source")) {
            registration_data["source"] = effective_registry_source(info);
        }

        bool is_collection_overwrite = is_model_collection_recipe(actual_recipe) &&
                                        model_data.contains("components");
        if (is_collection_overwrite) {
            if (auto err = validate_collection_request(model_name, registration_data)) {
                throw std::runtime_error(*err);
            }
            model_registered = false;
            LOG(INFO, "ModelManager") << "Overwriting collection: "
                                      << model_name << std::endl;
        } else if (actual_checkpoint.empty()) {
            actual_checkpoint = info.checkpoint();
            actual_recipe = info.recipe;
        } else {
            std::string conflict = describe_registration_conflict(info, registration_data);
            if (!conflict.empty()) {
                throw std::runtime_error(
                    "Model '" + model_name + "' is already registered with different "
                    "model metadata: " + conflict + ". Choose a different model name "
                    "for this registry checkpoint."
                );
            }
            if (actual_recipe.empty()) {
                actual_recipe = info.recipe;
            } else {
                model_registered = false;
            }
        }
    }

    // Parse checkpoint
    std::string repo_id = actual_checkpoint;
    std::string variant = "";

    size_t colon_pos = actual_checkpoint.find(':');
    if (colon_pos != std::string::npos) {
        repo_id = actual_checkpoint.substr(0, colon_pos);
        variant = actual_checkpoint.substr(colon_pos + 1);
    }

    // Register collections early — the fan-out below calls get_model_info().
    // Track that this call created the registration so component-resolution
    // failures can roll it back instead of leaving a broken entry behind.
    bool collection_registered_this_call = false;
    if (is_model_collection_recipe(actual_recipe) && is_user_model_name(model_name) && !model_registered) {
        register_user_model(model_name, registration_data);
        model_registered = true;
        collection_registered_this_call = true;
    }

    // Collections don't have their own backend — download each component instead.
    //
    // Persistence follows one rule, uniform across models and collections: a
    // registry entry stores what was *authored locally*; anything *fetched from
    // a remote registry* lives in the shared model-hub cache and is rebuilt on lookup. A regular model
    // persists its recipe/checkpoint but not its weights; an inline collection
    // persists its components (authored); a registry-backed collection persists only
    // its `checkpoint` pointer, with the component list rebuilt from the cached
    // manifest (fetched). The one exception: a fetched manifest registers its
    // components as user models so they're routable.
    if (is_model_collection_recipe(actual_recipe)) {
        // Cycle guard: re-entering the same collection on the current call
        // chain means the user registered a circular reference (e.g. user.A
        // includes user.B which includes user.A). Without this, the fan-out
        // would recurse until the stack or HTTP timeout gives out.
        if (!visited.insert(model_name).second) {
            throw std::runtime_error(
                "Cycle detected in collection components: '" + model_name +
                "' transitively references itself. Edit the offending collection "
                "in user_models.json to remove the cycle."
            );
        }

        auto info = get_model_info(model_name);
        std::vector<std::string> components = info.components;

        try {
            if (model_data.contains("models") && model_data["models"].is_array() &&
                model_data.contains("components") && model_data["components"].is_array()) {
                // Collection file import: the body carries each component's definition
                // inline in `models` (the exported-collection format). Register unknown
                // components from those definitions and canonicalize the list.
                components = register_components(model_data["components"], model_data["models"],
                                                effective_registry_source(info));

                // The early registration above persisted the raw component names from
                // the file; re-register with the canonical list so cache lookups
                // (check_component_downloaded, update_model_in_cache) match after a
                // rebuild. register_user_model drops the bulky `models` array and,
                // for a registry-backed collection (one with a checkpoint pointer), also
                // drops `components` so the cached manifest stays the sole source of
                // truth — only a pure inline collection persists its component list.
                if (is_user_model_name(model_name) && !components.empty()) {
                    json reg = registration_data;
                    reg["components"] = components;
                    register_user_model(model_name, reg);
                }
            } else if (!repo_id.empty() && (components.empty() || !do_not_upgrade)) {
                // Registry-backed collections keep their full definition — the component list
                // and each component's model object — in the configured registry as an exported
                // collection JSON. A non-empty checkpoint marks such a collection; fetch
                // the manifest to learn its components. On an explicit pull
                // (do_not_upgrade=false) always refresh so newly added components are
                // picked up.
                components = resolve_collection_components_from_manifest(
                    repo_id, effective_registry_source(info), do_not_upgrade);
            }

            if (components.empty()) {
                throw std::runtime_error("Collection '" + model_name + "' has no components defined");
            }
        } catch (...) {
            // Roll back the early registration when component resolution fails,
            // so a failed import does not persist a collection entry whose
            // components were never resolved. Downloads below are not rolled
            // back — a mid-download failure leaves a valid, re-pullable entry.
            if (collection_registered_this_call) {
                LOG(WARNING, "ModelManager") << "Component resolution failed; unregistering "
                    << "collection: " << model_name << std::endl;
                unregister_user_model(model_name);
            }
            throw;
        }
        LOG(INFO, "ModelManager") << "Downloading " << components.size()
                                  << " component(s) for collection: " << model_name << std::endl;

        // Wrap the callback so recursive per-component downloads don't each
        // emit a "complete" event — the SSE stream should only see one final
        // completion after every component finishes.
        //
        // Capture progress_callback by value (not by reference) so `forward`
        // owns a copy of the std::function. This keeps the callback usable
        // even if `forward` is ever copied or outlives this stack frame;
        // today it's only consumed synchronously below, but the defensive
        // copy is free (std::function is copyable) and removes a latent
        // dangling-reference hazard if the recursion pattern changes.
        DownloadProgressCallback forward = nullptr;
        if (progress_callback) {
            forward = [progress_callback](const DownloadProgress& p) -> bool {
                if (p.complete) return true;
                return progress_callback(p);
            };
        }

        for (const auto& component : components) {
            if (!model_exists(component)) {
                LOG(WARNING, "ModelManager") << "Skipping unknown component: " << component << std::endl;
                continue;
            }
            auto comp_info = get_model_info(component);
            if (comp_info.downloaded) {
                LOG(INFO, "ModelManager") << "Component already downloaded: " << component << std::endl;
                continue;
            }
            LOG(INFO, "ModelManager") << "Downloading component: " << component << std::endl;
            json comp_data = json::object();
            download_model(component, comp_data, do_not_upgrade, forward, visited);
        }

        // A registry-backed collection's in-memory components were empty until the
        // manifest was fetched above (build_cache populated them empty at startup
        // when no manifest was cached yet). Invalidate the cache so the next lookup
        // rebuilds the collection entry from the now-cached manifest — populating
        // its components and recomputing its downloaded status — instead of leaving
        // the stale empty-components/not-downloaded state until a restart.
        if (!repo_id.empty()) {
            std::lock_guard<std::mutex> lock(models_cache_mutex_);
            cache_valid_ = false;
        }

        // Emit a single completion event for the whole collection
        if (progress_callback) {
            DownloadProgress progress;
            progress.complete = true;
            progress.percent = 100;
            (void)progress_callback(progress);
        }
        // Scope cycle detection to the current recursion stack: a sibling branch
        // that legitimately revisits this collection (DAG, e.g. A->{B,C}->D
        // where D is itself a collection) must not be misread as a cycle.
        visited.erase(model_name);
        return;
    }

    // Check if this recipe is supported on the current system
    bool disable_filtering = false;
    if (auto* cfg = RuntimeConfig::global()) {
        disable_filtering = cfg->disable_model_filtering();
    }
    std::string unsupported_reason = SystemInfo::check_recipe_supported(actual_recipe);
    if (!unsupported_reason.empty() && !disable_filtering) {
        throw std::runtime_error(
            "Model '" + model_name + "' cannot be used on this system (recipe: " + actual_recipe + "): " +
            unsupported_reason
        );
    }

    LOG(INFO, "ModelManager") << "Downloading model: " << repo_id;
    if (!variant.empty()) {
        LOG(INFO, "ModelManager") << " (variant: " << variant << ")";
    }
    LOG(INFO, "ModelManager") << std::endl;

    // Check if offline mode
    if (auto* cfg = RuntimeConfig::global()) {
        if (cfg->offline()) {
            LOG(INFO, "ModelManager") << "Offline mode enabled, skipping download" << std::endl;
            return;
        }
    }

    // Persist registration and recipe options BEFORE the cache-first shortcut
    // below. A registration/import/overwrite that targets an already-downloaded
    // model must still update user_models.json and recipe_options.json. The
    // do_not_upgrade fast return only skips the remote-registry download/update
    // check — it must never skip the metadata the caller asked us to record,
    // otherwise an import that reports success would silently leave the old
    // checkpoints/options in place.
    if (is_user_model_name(model_name) && !model_registered) {
        register_user_model(model_name, registration_data);
    }

    auto model_info = get_model_info(model_name);

    if (model_data.contains("recipe_options")) {
        // Merge import recipe_options on top of the already-merged options
        // (which include image_defaults + JSON recipe_options + user-saved).
        // This preserves the full 3-level merge from add_model_to_cache while
        // layering in any import-specific overrides (e.g. sdcpp_args).
        json merged = model_info.recipe_options.to_json();
        for (auto& [key, value] : model_data["recipe_options"].items()) {
            merged[key] = value;
        }
        model_info.recipe_options = RecipeOptions(model_info.recipe, merged);
        save_model_options(model_info);
    }

    // CRITICAL: If do_not_upgrade=true AND model is already downloaded, skip the
    // remote-registry update check. Registration and recipe options were already
    // persisted above, so an import/overwrite still takes effect on disk.
    // The do_not_upgrade flag means:
    //   - Load/inference endpoints: Don't check the remote registry for updates (use cache if available)
    //   - Pull endpoint: Always check the recorded registry for the latest version (do_not_upgrade=false)
    if (do_not_upgrade && is_model_downloaded(model_name)) {
        LOG(INFO, "ModelManager") << "Model already downloaded and do_not_upgrade=true, using cached version" << std::endl;
        return;
    }

    download_registered_model(model_info, do_not_upgrade, progress_callback);
}

/**
 * Download everything from download manifest.
 */

struct HfFileMetadata {
    size_t size = 0;
    std::string content_id;
    std::string hash_algorithm;
    std::string hash_value;

    bool has_content_id() const {
        return !content_id.empty();
    }

    bool has_hash() const {
        return !hash_algorithm.empty() && !hash_value.empty();
    }
};

static std::string hf_file_metadata_key(const std::string& repo_id, const std::string& filename) {
    return repo_id + ':' + filename;
}

static HfFileMetadata hf_file_metadata_from_tree_file(const json& file) {
    HfFileMetadata entry;
    if (file.contains("size") && file["size"].is_number_unsigned()) {
        entry.size = file["size"].get<size_t>();
    }

    if (file.contains("lfs") && file["lfs"].is_object()) {
        const auto& lfs = file["lfs"];
        if (lfs.contains("size") && lfs["size"].is_number()) {
            entry.size = lfs["size"].get<size_t>();
        }
        if (lfs.contains("oid") && lfs["oid"].is_string()) {
            entry.hash_algorithm = "sha256";
            entry.hash_value = lfs["oid"].get<std::string>();
            entry.content_id = "lfs:" + entry.hash_value;
        }
        return entry;
    }

    if (file.contains("oid") && file["oid"].is_string()) {
        entry.hash_algorithm = "git-sha1";
        entry.hash_value = file["oid"].get<std::string>();
        entry.content_id = "git:" + entry.hash_value;
    }

    return entry;
}

static std::map<std::string, HfFileMetadata> fetch_hf_file_metadata_for_ref(
    const std::string& repo_id,
    const std::string& ref,
    const std::vector<std::string>& selected_files,
    const std::map<std::string, std::string>& headers) {
    std::map<std::string, HfFileMetadata> metadata;
    if (repo_id.empty() || ref.empty() || selected_files.empty()) {
        return metadata;
    }

    std::set<std::string> selected(selected_files.begin(), selected_files.end());
    std::set<std::string> subdirs_to_fetch;
    subdirs_to_fetch.insert("");

    for (const auto& filename : selected_files) {
        auto last_slash_pos = filename.rfind('/');
        if (last_slash_pos != std::string::npos) {
            subdirs_to_fetch.insert(filename.substr(0, last_slash_pos));
        }
    }

    std::string hf_endpoint = "https://huggingface.co";
    if (const char* configured = std::getenv("HF_ENDPOINT"); configured && configured[0]) {
        hf_endpoint = configured;
        while (!hf_endpoint.empty() && hf_endpoint.back() == '/') hf_endpoint.pop_back();
    }

    for (const auto& subdir : subdirs_to_fetch) {
        std::string tree_url = hf_endpoint + "/api/models/" + repo_id + "/tree/" + ref;
        if (!subdir.empty()) {
            tree_url += "/" + subdir;
        }

        auto tree_response = HttpClient::get(tree_url, headers);
        if (tree_response.status_code != 200) {
            LOG(DEBUG, "ModelManager") << "Could not fetch Hugging Face tree metadata for "
                                       << repo_id << " at " << ref << std::endl;
            continue;
        }

        auto tree_info = JsonUtils::parse(tree_response.body);
        if (!tree_info.is_array()) {
            continue;
        }

        for (const auto& file : tree_info) {
            if (!file.contains("path") || !file["path"].is_string()) {
                continue;
            }

            const std::string path = file["path"].get<std::string>();
            if (selected.find(path) == selected.end()) {
                continue;
            }

            metadata[hf_file_metadata_key(repo_id, path)] = hf_file_metadata_from_tree_file(file);
        }
    }

    return metadata;
}

static bool can_reuse_previous_hf_snapshot(
    const std::string& repo_id,
    const std::vector<std::string>& selected_files,
    const fs::path& previous_snapshot,
    const std::map<std::string, HfFileMetadata>& current_metadata,
    const std::map<std::string, HfFileMetadata>& previous_metadata) {
    if (repo_id.empty() || selected_files.empty() || previous_snapshot.empty() || !safe_exists(previous_snapshot)) {
        return false;
    }

    for (const auto& filename : selected_files) {
        const std::string key = hf_file_metadata_key(repo_id, filename);
        auto current_it = current_metadata.find(key);
        auto previous_it = previous_metadata.find(key);
        if (current_it == current_metadata.end() || previous_it == previous_metadata.end() ||
            !current_it->second.has_content_id() || !previous_it->second.has_content_id() ||
            current_it->second.content_id != previous_it->second.content_id) {
            return false;
        }

        fs::path previous_file = previous_snapshot / path_from_utf8(filename);
        fs::path previous_partial = path_from_utf8(path_to_utf8(previous_file) + ".partial");
        std::error_code ec;
        if (!fs::is_regular_file(previous_file, ec) || safe_exists(previous_partial)) {
            return false;
        }

        if (current_it->second.size > 0) {
            const auto previous_size = fs::file_size(previous_file, ec);
            if (ec || previous_size != current_it->second.size) {
                return false;
            }
        }
    }

    return true;
}

static void remove_unused_hf_snapshot(const fs::path& cache_path,
                                      const std::string& snapshot_path,
                                      const std::string& active_ref) {
    if (cache_path.empty() || snapshot_path.empty() || active_ref.empty()) {
        return;
    }

    fs::path unused_snapshot = path_from_utf8(snapshot_path);
    fs::path active_snapshot = cache_path / "snapshots" / active_ref;
    std::error_code ec;
    if (!safe_exists(unused_snapshot) || fs::equivalent(unused_snapshot, active_snapshot, ec)) {
        return;
    }
    ec.clear();
    fs::remove_all(unused_snapshot, ec);
    if (ec) {
        LOG(WARNING, "ModelManager") << "Could not remove unused Hugging Face snapshot: "
                                     << path_to_utf8(unused_snapshot)
                                     << " (" << ec.message() << ")" << std::endl;
    }
}

void ModelManager::download_from_manifest(const json& manifest, std::map<std::string, std::string>& headers, DownloadProgressCallback progress_callback) {
    // Download each file with robust retry and resume support
    int file_index = 0;
    std::string download_path = manifest["download_path"].get<std::string>();
    int total_files = manifest["files_count"].get<int>();


    // Compute total download size across all files for accurate progress reporting
    size_t total_download_size = 0;
    for (const auto& file_desc : manifest["files"]) {
        total_download_size += file_desc["size"].get<size_t>();
    }

    // Pre-flight disk space check: fail fast before downloading anything.
    // Subtract bytes already on disk (completed files + partial downloads)
    // so resumed downloads aren't falsely rejected. Group by target path first,
    // then fold paths that share the same space info so shared filesystems are
    // checked against the combined remaining download size.
    {
        std::map<std::string, size_t> bytes_needed_by_target_path;
        for (const auto& file_desc : manifest["files"]) {
            std::string filename = file_desc["name"].get<std::string>();
            size_t file_size = file_desc["size"].get<size_t>();
            // Per-file download_path for multi-repo models; fall back to top-level
            std::string file_download_path = file_desc.value("download_path", download_path);
            std::string output_path = file_download_path + "/" + filename;
            std::string partial_path = output_path + ".partial";
            fs::path output_path_fs = path_from_utf8(output_path);
            fs::path partial_path_fs = path_from_utf8(partial_path);
            size_t bytes_needed_for_file = file_size;
            if (safe_exists(output_path_fs) && !safe_exists(partial_path_fs)) {
                bytes_needed_for_file = 0;
            } else if (safe_exists(partial_path_fs)) {
                // Cap credit to manifest size — partial can't save more than the file costs
                size_t partial_size = fs::file_size(partial_path_fs);
                size_t bytes_already_on_disk = (std::min)(partial_size, file_size);
                // Clamp to zero: manifest can contain size=0 entries while partials exist.
                bytes_needed_for_file = (file_size > bytes_already_on_disk)
                    ? file_size - bytes_already_on_disk
                    : 0;
            }

            if (bytes_needed_for_file > 0) {
                bytes_needed_by_target_path[file_download_path] += bytes_needed_for_file;
            }
        }

        using SpaceSignature = std::tuple<uintmax_t, uintmax_t, uintmax_t>;
        std::map<SpaceSignature, std::pair<size_t, std::string>> bytes_needed_by_filesystem;
        for (const auto& [target_path, bytes_needed] : bytes_needed_by_target_path) {
            std::error_code ec;
            auto si = fs::space(path_from_utf8(target_path), ec);
            if (ec) {
                continue;
            }

            auto key = std::make_tuple(si.capacity, si.free, si.available);
            auto& grouped_entry = bytes_needed_by_filesystem[key];
            grouped_entry.first += bytes_needed;
            if (grouped_entry.second.empty()) {
                grouped_entry.second = target_path;
            }
        }

        for (const auto& [space_signature, grouped_entry] : bytes_needed_by_filesystem) {
            size_t bytes_needed = grouped_entry.first;
            uintmax_t available = std::get<2>(space_signature);
            const std::string& target_path = grouped_entry.second;
            if (bytes_needed <= available) {
                continue;
            }

            std::ostringstream oss;
            oss << "Insufficient disk space: download requires "
                << std::fixed << std::setprecision(1)
                << (bytes_needed / (1024.0 * 1024.0 * 1024.0)) << " GB but only "
                << (available / (1024.0 * 1024.0 * 1024.0)) << " GB is available on "
                << target_path;
            throw std::runtime_error(oss.str());
        }
    }

    for (const auto& file_desc : manifest["files"]) {
        file_index++;
        std::string filename = file_desc["name"].get<std::string>();
        std::string file_url = file_desc["url"].get<std::string>();
        size_t file_size = file_desc["size"].get<size_t>();
        // Per-file download_path for multi-repo models; fall back to top-level for old manifests
        std::string file_download_path = file_desc.value("download_path", download_path);
        std::string output_path = file_download_path + "/" + filename;

        // Create parent directory for file (handles folders in filenames)
        ensure_create_directories(fs::path(output_path).parent_path());

        LOG(INFO, "ModelManager") << "Downloading: " << filename << "..." << std::endl;

        // Send progress update if callback provided (and check for cancellation)
        if (progress_callback) {
            DownloadProgress progress;
            progress.file = filename;
            progress.file_index = file_index;
            progress.total_files = total_files;
            progress.bytes_downloaded = 0;
            progress.bytes_total = file_size;
            progress.total_download_size = total_download_size;
            progress.percent = 0;
            if (!progress_callback(progress)) {
                LOG(INFO, "ModelManager") << "Download cancelled by client" << std::endl;
                throw std::runtime_error("Download cancelled");
            }
        }

        // Detect bytes already on disk before downloading (for resume/skip tracking)
        size_t bytes_on_disk = 0;
        std::string partial_path = output_path + ".partial";

        // GGUF models are consumed directly by llama-server. If a previous
        // download accidentally saved an HTML/error/pointer file, the backend
        // later fails with the unhelpful "llama-server failed to start".
        // Reject that cache entry before HttpClient can treat the final path
        // as already complete. SHA validation still remains the primary check
        // when Hugging Face exposes an LFS object id.
        if (gguf_reader_detail::ends_with_ignore_case(filename, ".gguf") &&
            fs::exists(output_path) && !fs::exists(partial_path) &&
            !gguf_reader_detail::has_gguf_magic(output_path)) {
            LOG(WARNING, "ModelManager") << "Removing invalid GGUF cache file before download: "
                                         << filename << std::endl;
            std::error_code remove_ec;
            fs::remove(path_from_utf8(output_path), remove_ec);
            if (remove_ec) {
                throw std::runtime_error(
                    "Invalid GGUF cache file could not be removed: " + output_path +
                    " (" + remove_ec.message() + ")");
            }
        }

        if (fs::exists(output_path) && !fs::exists(partial_path)) {
            bytes_on_disk = file_size;  // File already complete
        } else if (fs::exists(partial_path)) {
            bytes_on_disk = fs::file_size(partial_path);  // Partial download
        }

        utils::DownloadOptions download_opts;
        download_opts.max_retries = 10;
        download_opts.initial_retry_delay_ms = 2000;
        download_opts.max_retry_delay_ms = 120000;
        download_opts.resume_partial = true;
        download_opts.low_speed_limit = 1000;
        download_opts.low_speed_time = 60;
        download_opts.connect_timeout = 60;
        if (file_desc.contains("hash") && file_desc["hash"].is_object()) {
            const auto& hash = file_desc["hash"];
            if (hash.contains("algorithm") && hash["algorithm"].is_string() &&
                hash.contains("value") && hash["value"].is_string()) {
                download_opts.expected_hash_algorithm = hash["algorithm"].get<std::string>();
                download_opts.expected_hash = hash["value"].get<std::string>();
            }
        } else if (file_desc.contains("sha256") && file_desc["sha256"].is_string()) {
            // Backward-compatible manifest shape for tests / hand-authored manifests.
            download_opts.expected_hash_algorithm = "sha256";
            download_opts.expected_hash = file_desc["sha256"].get<std::string>();
        }

        // Create progress callback that reports to both console and SSE callback
        // Returns bool: true = continue, false = cancel
        utils::ProgressCallback http_progress_cb;
        if (progress_callback) {
            http_progress_cb = [&, total_download_size, bytes_on_disk](size_t downloaded, size_t total) -> bool {
                DownloadProgress progress;
                progress.file = filename;
                progress.file_index = file_index;
                progress.total_files = total_files;
                progress.bytes_downloaded = downloaded;
                progress.bytes_total = total;
                progress.total_download_size = total_download_size;
                progress.bytes_previously_downloaded = bytes_on_disk;
                progress.percent = (total > 0) ? static_cast<int>((downloaded * 100) / total) : 0;
                return progress_callback(progress);  // Propagate cancellation
            };
        } else {
            // Default console progress callback (never cancels)
            http_progress_cb = utils::create_throttled_progress_callback();
        }

        auto result = HttpClient::download_file(
            file_url,
            output_path,
            http_progress_cb,
            headers,
            download_opts
        );

        // Check if download was cancelled
        if (result.cancelled) {
            LOG(INFO, "ModelManager") << "Download cancelled by client" << std::endl;
            throw std::runtime_error("Download cancelled");
        }

        if (result.success) {
            LOG(INFO, "ModelManager") << "Downloaded: " << filename << std::endl;

            // Emit completion event for already-complete files that were skipped
            // (download_file returns bytes_downloaded=0 when file already exists)
            if (result.bytes_downloaded == 0 && progress_callback) {
                DownloadProgress progress;
                progress.file = filename;
                progress.file_index = file_index;
                progress.total_files = total_files;
                progress.bytes_downloaded = file_size;
                progress.bytes_total = file_size;
                progress.total_download_size = total_download_size;
                progress.bytes_previously_downloaded = file_size;  // Entire file was pre-existing
                progress.percent = 100;
                (void)progress_callback(progress);
            }
        } else {
            // Build a detailed error message
            std::ostringstream error_msg;
            error_msg << "Failed to download file: " << filename << "\n";
            error_msg << "URL: " << file_url << "\n";
            error_msg << result.error_message;

            // If there's a partial file, provide helpful information
            if (fs::exists(output_path)) {
                size_t partial_size = fs::file_size(output_path);
                if (partial_size > 0) {
                    error_msg << "\n\n[INFO] Partial download preserved at: " << output_path;
                    error_msg << "\n[INFO] Partial size: " << std::fixed << std::setprecision(1)
                                << (partial_size / (1024.0 * 1024.0)) << " MB";
                    error_msg << "\n[INFO] Run the command again to resume from where it left off.";
                }
            }

            throw std::runtime_error(error_msg.str());
        }
    }

    // Validate all expected files exist after download
    LOG(INFO, "ModelManager") << "Validating downloaded files..." << std::endl;
    bool all_valid = true;
    for (const auto& file_desc : manifest["files"]) {
        std::string filename = file_desc["name"].get<std::string>();
        size_t expected_size = file_desc["size"].get<size_t>();

        std::string file_download_path = file_desc.value("download_path", download_path);
        std::string expected_path = file_download_path + "/" + filename;
        std::string partial_path = expected_path + ".partial";

        // Check for .partial file (incomplete download)
        if (fs::exists(partial_path)) {
            if (fs::exists(expected_path)) {
                // Final file exists alongside stale .partial — clean up the leftover
                LOG(INFO, "ModelManager") << "Removing stale partial file: " << filename << ".partial" << std::endl;
                std::error_code ec;
                fs::remove(partial_path, ec);
            } else {
                all_valid = false;
                LOG(ERROR, "ModelManager") << "Incomplete file found: " << filename << ".partial" << std::endl;
                continue;
            }
        }

        if (!fs::exists(expected_path)) {
            all_valid = false;
            LOG(ERROR, "ModelManager") << "Missing file: " << filename << std::endl;
            continue;
        }

        // Verify file size if we have expected size from tree API
        if (expected_size > 0) {
            size_t actual_size = fs::file_size(expected_path);
            if (actual_size != expected_size) {
                // Log mismatch but don't fail — tree API sizes can differ from
                // actual LFS object sizes in some edge cases
                LOG(WARNING, "ModelManager") << "Size note for " << filename
                            << ": tree API reports " << expected_size
                            << " bytes, actual " << actual_size << " bytes" << std::endl;
            }
        }

        // llama.cpp fails late and opaquely when a cached GGUF path points to
        // an HTML/error/pointer file. Surface that as download validation
        // failure instead, and remove the invalid final file so the next pull
        // starts fresh.
        if (gguf_reader_detail::ends_with_ignore_case(filename, ".gguf") && !gguf_reader_detail::has_gguf_magic(expected_path)) {
            all_valid = false;
            LOG(ERROR, "ModelManager") << "Invalid GGUF file: " << filename
                                       << " (missing GGUF magic header)" << std::endl;
            std::error_code remove_ec;
            fs::remove(path_from_utf8(expected_path), remove_ec);
            if (remove_ec) {
                LOG(ERROR, "ModelManager") << "Failed to remove invalid GGUF file: "
                                           << remove_ec.message() << std::endl;
            }
            continue;
        }
    }

    if (!all_valid) {
        throw std::runtime_error(
            "Download validation failed. Some files are incomplete or missing. "
            "Run the command again to resume."
        );
    }
}

// Download model files from the configured remote registry.
// =========================================================
// This function always queries the selected registry for the current file tree.
// The caller is responsible for applying do_not_upgrade/offline policy first.
//
// The caller (download_model) is responsible for checking do_not_upgrade and
// calling is_model_downloaded() before invoking this function.
//
// Download capabilities by backend:
//   - Lemonade Router (ModelManager): downloads non-FLM registry models
//   - FLM backend: ✅ Downloads FLM models via 'flm pull' command
//   - llama-server backend: ❌ Cannot download (expects GGUF files pre-cached)
//   - ryzenai-server backend: ❌ Cannot download (expects ONNX files pre-cached)
void ModelManager::download_from_registry(const ModelInfo& info,
                                          DownloadProgressCallback progress_callback) {
    const std::string main_repo_id = checkpoint_to_repo_id(info.checkpoint("main"));
    const std::string main_variant = checkpoint_to_variant(info.checkpoint("main"));
    if (main_repo_id.empty()) {
        throw std::runtime_error("Model checkpoint does not contain a repository id");
    }

    const std::string source_name = effective_registry_source(info);
    const auto source = parse_remote_registry_source(source_name);
    const auto& registry = model_registry(source);
    const std::string source_display = remote_registry_display_name(source);
    std::map<std::string, std::string> headers = registry.auth_headers();

    const fs::path cache_root = path_from_utf8(get_hf_cache_dir());
    ensure_create_directories(cache_root);

    LOG(INFO, "ModelManager") << "Fetching repository file list from "
                               << source_display << ": " << main_repo_id << std::endl;

    std::map<std::string, RegistryRepository> repositories;
    repositories.emplace(main_repo_id, registry.fetch_repository(main_repo_id));

    std::map<std::string, std::vector<std::string>> files_to_download;
    std::vector<std::string> main_repo_files;
    for (const auto& file : repositories.at(main_repo_id).files) {
        if (!file.directory) main_repo_files.push_back(file.path);
    }
    LOG(INFO, "ModelManager") << "Repository contains " << main_repo_files.size()
                               << " files" << std::endl;

    auto backend_files =
        backends::ops_for(info.recipe)->select_checkpoint_files(main_variant, main_repo_files);

    if (!main_variant.empty()) {
        auto ends_with = [](const std::string& value, const std::string& suffix) {
            return value.size() >= suffix.size() &&
                   value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        const bool direct_file = ends_with(main_variant, ".safetensors") ||
                                 ends_with(main_variant, ".pth") ||
                                 ends_with(main_variant, ".ckpt");

        if (direct_file) {
            if (std::find(main_repo_files.begin(), main_repo_files.end(), main_variant) ==
                main_repo_files.end()) {
                throw std::runtime_error("Model file not found in " + source_display +
                                         " repository: " + main_variant);
            }
            files_to_download[main_repo_id].push_back(main_variant);
        } else if (backend_files) {
            files_to_download[main_repo_id] = std::move(*backend_files);
        } else {
            GGUFFiles gguf_files = identify_gguf_models(main_repo_id, main_variant, main_repo_files);
            std::unordered_set<std::string> added_files;
            for (const auto& [key, filename] : gguf_files.core_files) {
                (void)key;
                files_to_download[main_repo_id].push_back(filename);
                added_files.insert(filename);
            }
            for (const auto& filename : gguf_files.sharded_files) {
                if (!added_files.count(filename)) files_to_download[main_repo_id].push_back(filename);
            }
        }

        for (const std::string& config_file : {
                 "config.json", "tokenizer.json", "tokenizer_config.json", "tokenizer.model"}) {
            if (std::find(main_repo_files.begin(), main_repo_files.end(), config_file) !=
                    main_repo_files.end() &&
                std::find(files_to_download[main_repo_id].begin(),
                          files_to_download[main_repo_id].end(), config_file) ==
                    files_to_download[main_repo_id].end()) {
                files_to_download[main_repo_id].push_back(config_file);
            }
        }
    } else if (backend_files) {
        files_to_download[main_repo_id] = std::move(*backend_files);
    } else {
        files_to_download[main_repo_id] = main_repo_files;
    }

    // Auxiliary checkpoints inherit the model-level registry source. This is
    // intentional: a registration has one provenance and update domain.
    for (const auto& [type, checkpoint] : info.checkpoints) {
        if (type == "main" || type == "npu_cache") continue;
        const std::string repo_id = checkpoint_to_repo_id(checkpoint);
        const std::string variant = checkpoint_to_variant(checkpoint);
        if (repo_id.empty() || variant.empty()) {
            throw std::runtime_error("Additional checkpoints must contain an exact repository variant");
        }
        if (!repositories.count(repo_id)) {
            repositories.emplace(repo_id, registry.fetch_repository(repo_id));
        }
        const auto& repo = repositories.at(repo_id);
        const bool exists = std::any_of(repo.files.begin(), repo.files.end(),
            [&](const RegistryFile& file) { return !file.directory && file.path == variant; });
        if (!exists) {
            throw std::runtime_error("Additional checkpoint file not found on " +
                                     source_display + ": " + repo_id + ":" + variant);
        }
        files_to_download[repo_id].push_back(variant);
    }

    int total_files = 0;
    LOG(INFO, "ModelManager") << "Identified files to download:" << std::endl;
    for (const auto& [repo_id, files] : files_to_download) {
        for (const auto& filename : files) {
            ++total_files;
            LOG(INFO, "ModelManager") << "  - " << source_name << ':' << repo_id
                                       << ':' << filename << std::endl;
        }
    }
    if (total_files == 0) {
        throw std::runtime_error("No files selected for download from " + source_display +
                                 " repository " + main_repo_id);
    }

    std::map<std::string, fs::path> repo_cache_paths;
    std::map<std::string, std::string> repo_previous_refs;
    std::map<std::string, std::string> repo_snapshot_paths;
    std::map<std::string, std::string> repo_snapshot_ids;
    std::map<std::string, std::string> repo_download_paths;

    for (const auto& [repo_id, files] : files_to_download) {
        if (files.empty()) continue;
        const auto& repo = repositories.at(repo_id);
        fs::path repo_cache = cache_root / repo_id_to_cache_dir_name(repo_id, source_name);
        ensure_create_directories(repo_cache);
        const std::string previous_ref = read_hf_ref_main(repo_cache);
        fs::path snapshot = repo_cache / "snapshots" / repo.snapshot_id;
        ensure_create_directories(snapshot);

        repo_cache_paths[repo_id] = repo_cache;
        repo_previous_refs[repo_id] = previous_ref;
        repo_snapshot_paths[repo_id] = path_to_utf8(snapshot);
        repo_snapshot_ids[repo_id] = repo.snapshot_id;
        repo_download_paths[repo_id] = path_to_utf8(snapshot);
    }

    // Preserve the existing HF optimization: if every selected artifact is
    // byte-identical at the new commit, keep refs/main on the previous snapshot.
    // ModelScope snapshots are tree fingerprints and currently advance together
    // with the reported tree, because its branch API has no HF-style commit pin.
    std::set<std::string> repos_reusing_previous_snapshot;
    if (source == RemoteRegistrySource::HuggingFace) {
        for (const auto& [repo_id, files] : files_to_download) {
            if (files.empty()) continue;
            const std::string current_ref = repo_snapshot_ids.at(repo_id);
            const std::string previous_ref = repo_previous_refs.at(repo_id);
            if (previous_ref.empty() || previous_ref == current_ref) continue;

            const auto current_metadata =
                fetch_hf_file_metadata_for_ref(repo_id, current_ref, files, headers);
            const auto previous_metadata =
                fetch_hf_file_metadata_for_ref(repo_id, previous_ref, files, headers);
            const fs::path previous_snapshot =
                repo_cache_paths.at(repo_id) / "snapshots" / previous_ref;
            if (can_reuse_previous_hf_snapshot(repo_id, files, previous_snapshot,
                                               current_metadata, previous_metadata)) {
                repo_download_paths[repo_id] = path_to_utf8(previous_snapshot);
                repos_reusing_previous_snapshot.insert(repo_id);
                LOG(INFO, "ModelManager") << "Keeping active Hugging Face snapshot for "
                    << repo_id << " at " << previous_ref
                    << " because selected artifacts are unchanged in " << current_ref
                    << std::endl;
            }
        }
    }

    json manifest;
    manifest["repo_id"] = main_repo_id;
    manifest["registry_source"] = source_name;
    manifest["commit_hash"] = repo_snapshot_ids.at(main_repo_id);
    manifest["download_path"] = repo_snapshot_paths.at(main_repo_id);
    manifest["files_count"] = total_files;
    manifest["files"] = json::array();

    for (const auto& [repo_id, files] : files_to_download) {
        const auto& repo = repositories.at(repo_id);
        std::map<std::string, RegistryFile> metadata;
        for (const auto& file : repo.files) metadata[file.path] = file;

        for (const auto& filename : files) {
            json entry;
            entry["name"] = filename;
            entry["url"] = registry.resolve_file_url(repo_id, repo.revision, filename);
            entry["download_path"] = repo_download_paths.at(repo_id);
            const auto it = metadata.find(filename);
            entry["size"] = it == metadata.end() ? 0 : it->second.size;
            if (it != metadata.end() && !it->second.hash.empty()) {
                entry["hash"] = {
                    {"algorithm", it->second.hash_algorithm},
                    {"value", it->second.hash}
                };
            }
            manifest["files"].push_back(std::move(entry));
        }
    }

    const fs::path manifest_path =
        path_from_utf8(repo_snapshot_paths.at(main_repo_id)) / ".download_manifest.json";
    JsonUtils::save_to_file(manifest, path_to_utf8(manifest_path));
    download_from_manifest(manifest, headers, progress_callback);
    std::error_code manifest_ec;
    fs::remove(manifest_path, manifest_ec);

    for (const auto& [repo_id, snapshot_id] : repo_snapshot_ids) {
        const fs::path& cache_path = repo_cache_paths.at(repo_id);
        if (repos_reusing_previous_snapshot.count(repo_id)) {
            const std::string& previous_ref = repo_previous_refs.at(repo_id);
            write_hf_ref_main(cache_path, previous_ref);
            remove_unused_hf_snapshot(cache_path, repo_snapshot_paths.at(repo_id), previous_ref);
        } else {
            write_hf_ref_main(cache_path, snapshot_id);
        }

        const auto& repo = repositories.at(repo_id);
        json provenance = {
            {"source", source_name},
            {"repo_id", repo_id},
            {"revision", repo.revision},
            {"snapshot_id", repos_reusing_previous_snapshot.count(repo_id)
                                ? repo_previous_refs.at(repo_id) : snapshot_id}
        };
        JsonUtils::save_to_file(provenance,
            path_to_utf8(cache_path / ".lemonade_registry.json"));
    }

    if (progress_callback) {
        DownloadProgress progress;
        progress.complete = true;
        progress.file_index = total_files;
        progress.total_files = total_files;
        progress.percent = 100;
        (void)progress_callback(progress);
    }

    LOG(INFO, "ModelManager") << "All files downloaded and validated from "
                               << source_display << std::endl;
    LOG(INFO, "ModelManager") << "Download location: "
        << repo_download_paths.at(main_repo_id) << std::endl;
}


void ModelManager::delete_model(const std::string& model_name) {
    auto info = get_model_info(model_name);
    std::string canonical_model_name = info.model_name;

    LOG(INFO, "ModelManager") << "Deleting model: " << canonical_model_name << std::endl;
    LOG(INFO, "ModelManager") << "Checkpoint: " << info.checkpoint() << std::endl;
    LOG(INFO, "ModelManager") << "Recipe: " << info.recipe << std::endl;

    // Handle extra models (from --extra-models-dir) - these are user-managed external files
    if (canonical_model_name.substr(0, 6) == "extra.") {
        throw std::runtime_error("Cannot delete extra models via API. Models in --extra-models-dir are user-managed. "
                                 "Delete the file directly from: " + info.checkpoint());
    }

    // FLM models have no local HF cache; deletion is the backend's `flm remove`.
    if (info.recipe == "flm") {
        backends::fastflowlm::flm_remove(info.checkpoint());

        // Remove from user models if it's a user model
        if (is_user_model_name(canonical_model_name)) {
            std::lock_guard<std::mutex> lock(models_cache_mutex_);
            json updated_user_models = load_optional_json(get_user_models_file());
            if (!updated_user_models.is_object()) {
                updated_user_models = json::object();
            }
            updated_user_models.erase(strip_user_model_prefix(canonical_model_name));
            save_user_models(updated_user_models);
            user_models_ = std::move(updated_user_models);
            cache_valid_ = false;
            LOG(INFO, "ModelManager") << "✓ Removed from user_models.json" << std::endl;
        }

        // Remove from cache after successful deletion
        remove_model_from_cache(canonical_model_name);

        return;
    }

    // Use resolved_path to find the model directory to delete.
    // Cancelled or interrupted downloads may not have a resolved model path yet,
    // but they can still leave resumable .partial files and manifests in the HF
    // cache. Clean those artifacts before falling back to registry-only cleanup.
    if (info.resolved_path().empty()) {
        bool removed_incomplete_cache = cleanup_incomplete_hf_model_cache(
            info,
            canonical_model_name,
            models_cache_
        );

        if (!removed_incomplete_cache) {
            LOG(INFO, "ModelManager") << "Model not downloaded, removing from registry only" << std::endl;
        }

        if (is_user_model_name(canonical_model_name)) {
            std::lock_guard<std::mutex> lock(models_cache_mutex_);
            json updated_user_models = load_optional_json(get_user_models_file());
            if (!updated_user_models.is_object()) {
                updated_user_models = json::object();
            }
            updated_user_models.erase(strip_user_model_prefix(canonical_model_name));
            save_user_models(updated_user_models);
            user_models_ = std::move(updated_user_models);
            cache_valid_ = false;
            LOG(INFO, "ModelManager") << "✓ Removed from user_models.json" << std::endl;
        }

        remove_model_from_cache(canonical_model_name);
        LOG(INFO, "ModelManager") << "Successfully removed model from registry: " << canonical_model_name << std::endl;
        return;
    }

    // Find the models--* directory from resolved_path
    // resolved_path could be a file or directory, we need to find the models-- ancestor
    fs::path path_obj(path_from_utf8(info.resolved_path()));
    std::string model_cache_path;

    // Walk up the directory tree to find models--* directory
    while (!path_obj.empty() && path_obj.has_filename()) {
        std::string dirname = path_obj.filename().string();
        if (dirname.rfind("models--", 0) == 0 ||
            dirname.rfind("modelscope--models--", 0) == 0) {
            model_cache_path = path_to_utf8(path_obj);
            break;
        }
        path_obj = path_obj.parent_path();
    }

    if (model_cache_path.empty()) {
        throw std::runtime_error("Could not find models-- directory in path: " + info.resolved_path());
    }

    LOG(INFO, "ModelManager") << "Cache path: " << model_cache_path << std::endl;

    fs::path model_cache_path_fs = path_from_utf8(model_cache_path);
    std::string main_repo = checkpoint_to_repo_id(info.checkpoint("main"));

    // Check if the main repo is shared with another model
    bool main_shared = is_repo_shared(main_repo, effective_registry_source(info), canonical_model_name, models_cache_);

    if (!main_shared) {
        // No other model uses this repo — safe to delete the entire directory
        if (fs::exists(model_cache_path_fs)) {
            LOG(INFO, "ModelManager") << "Removing directory..." << std::endl;
            fs::remove_all(model_cache_path_fs);
            LOG(INFO, "ModelManager") << "✓ Deleted model files: " << canonical_model_name << std::endl;
        } else {
            LOG(INFO, "ModelManager") << "Warning: Model cache directory not found (may already be deleted)" << std::endl;
        }
    } else {
        // Shared repo — only delete this model's specific resolved variant path
        LOG(INFO, "ModelManager") << "Main repo " << main_repo
                    << " is shared with other models, deleting variant path only" << std::endl;
        std::string rpath = info.resolved_path("main");
        if (!rpath.empty()) {
            fs::path variant_path = path_from_utf8(rpath);
            if (safe_exists(variant_path)) {
                // Delete the whole resolved path, not just regular files
                // and clean any HF symlink blobs before removing the snapshot entries.
                cleanup_orphaned_blobs_under(variant_path, model_cache_path_fs);
                remove_resolved_path_or_throw(variant_path, "variant path");
                cleanup_empty_parents(variant_path, model_cache_path_fs);
            }
        }
        LOG(INFO, "ModelManager") << "✓ Deleted variant for: " << canonical_model_name << std::endl;
    }

    // Clean up non-main checkpoint files in their own repo dirs (multi-repo models)
    // Only delete if no other model in the registry references the same repo
    for (const auto& [type, checkpoint] : info.checkpoints) {
        if (type == "main" || type == "npu_cache") continue;

        std::string cp_repo = checkpoint_to_repo_id(checkpoint);
        if (cp_repo.empty() || cp_repo == main_repo) continue;

        if (is_repo_shared(cp_repo, effective_registry_source(info), canonical_model_name, models_cache_)) {
            LOG(INFO, "ModelManager") << "Keeping shared repo " << cp_repo
                        << " (used by other models)" << std::endl;
            continue;
        }

        // Not shared — safe to delete the entire repo directory
        std::string cp_cache_dir = get_hf_cache_dir() + "/" + repo_id_to_cache_dir_name(cp_repo, effective_registry_source(info));
        fs::path cp_cache_path = path_from_utf8(cp_cache_dir);
        if (fs::exists(cp_cache_path)) {
            LOG(INFO, "ModelManager") << "Removing non-main repo directory: " << cp_cache_dir << std::endl;
            fs::remove_all(cp_cache_path);
        }
    }

    // Remove from user models if it's a user model
    if (is_user_model_name(canonical_model_name)) {
        std::lock_guard<std::mutex> lock(models_cache_mutex_);
        json updated_user_models = load_optional_json(get_user_models_file());
        if (!updated_user_models.is_object()) {
            updated_user_models = json::object();
        }
        updated_user_models.erase(strip_user_model_prefix(canonical_model_name));
        save_user_models(updated_user_models);
        user_models_ = std::move(updated_user_models);
        cache_valid_ = false;
        LOG(INFO, "ModelManager") << "✓ Removed from user_models.json" << std::endl;
    }

    // Remove from cache after successful deletion
    remove_model_from_cache(canonical_model_name);
}

json ModelManager::cleanup_orphaned_cache(bool dry_run) {
    build_cache();

    std::string hf_cache = get_hf_cache_dir();
    json orphaned_files = json::array();
    size_t total_bytes = 0;

    std::lock_guard<std::mutex> lock(models_cache_mutex_);

    // Find multi-repo models where non-main checkpoints reference different repos
    for (const auto& [name, info] : models_cache_) {
        if (info.checkpoints.size() <= 1) continue;

        std::string main_repo = checkpoint_to_repo_id(info.checkpoint("main"));
        std::string main_cache = hf_cache + "/" + repo_id_to_cache_dir_name(main_repo, effective_registry_source(info));

        for (const auto& [type, checkpoint] : info.checkpoints) {
            if (type == "main" || type == "npu_cache") continue;

            std::string cp_repo = checkpoint_to_repo_id(checkpoint);
            if (cp_repo == main_repo) continue;  // Same repo, no orphan possible

            std::string variant = checkpoint_to_variant(checkpoint);
            if (variant.empty()) continue;

            // Check if file exists in main repo dir (orphaned location)
            fs::path main_cache_fs = path_from_utf8(main_cache);
            if (!fs::exists(main_cache_fs)) continue;

            // Search for the variant file in the main repo's cache
            for (const auto& entry : fs::recursive_directory_iterator(main_cache_fs)) {
                if (!entry.is_regular_file()) continue;

                std::string filename = entry.path().filename().string();
                // Match by filename for simple variants, or by path suffix for nested variants
                bool matches = (filename == variant) ||
                    (variant.find('/') != std::string::npos &&
                     path_to_utf8(entry.path()).find(variant) != std::string::npos);

                if (matches) {
                    size_t file_size = fs::file_size(entry.path());
                    std::string file_path = path_to_utf8(entry.path());

                    orphaned_files.push_back({
                        {"path", file_path},
                        {"size", file_size},
                        {"model", name},
                        {"type", type},
                        {"belongs_to", cp_repo}
                    });
                    total_bytes += file_size;

                    if (!dry_run) {
                        LOG(INFO, "ModelManager") << "Removing orphaned file: " << file_path << std::endl;
                        fs::remove(entry.path());
                    }
                    break;  // Found the orphan for this checkpoint
                }
            }
        }
    }

    json result;
    result["orphaned_files"] = orphaned_files;
    result["total_bytes"] = total_bytes;
    result["dry_run"] = dry_run;

    LOG(INFO, "ModelManager") << "Cache cleanup: found " << orphaned_files.size()
                << " orphaned file(s), " << (total_bytes / (1024 * 1024)) << " MB"
                << (dry_run ? " (dry run)" : " (deleted)") << std::endl;

    return result;
}

ModelInfo ModelManager::get_model_info(const std::string& model_name) {
    // Build cache if needed
    build_cache();

    // O(1) lookup in cache
    {
        std::lock_guard<std::mutex> lock(models_cache_mutex_);
        auto alias_it = public_model_aliases_.find(model_name);
        std::string canonical_name = alias_it != public_model_aliases_.end() ? alias_it->second : model_name;
        auto it = models_cache_.find(canonical_name);
        if (it != models_cache_.end()) {
            return it->second;
        }
    }

    if (refresh_user_models_from_disk_for_lookup(model_name)) {
        std::lock_guard<std::mutex> lock(models_cache_mutex_);
        auto alias_it = public_model_aliases_.find(model_name);
        std::string canonical_name = alias_it != public_model_aliases_.end() ? alias_it->second : model_name;
        auto it = models_cache_.find(canonical_name);
        if (it != models_cache_.end()) {
            return it->second;
        }
    }

    throw std::runtime_error("Model not found: " + model_name);
}

std::string ModelManager::resolve_model_name(const std::string& model_name) {
    build_cache();

    std::lock_guard<std::mutex> lock(models_cache_mutex_);
    auto it = public_model_aliases_.find(model_name);
    return it != public_model_aliases_.end() ? it->second : model_name;
}

std::string ModelManager::get_public_model_name(const std::string& model_name) {
    build_cache();

    std::lock_guard<std::mutex> lock(models_cache_mutex_);
    auto it = canonical_public_names_.find(model_name);
    return it != canonical_public_names_.end() ? it->second : model_name;
}

bool ModelManager::model_exists(const std::string& model_name) {
    // Build cache if needed
    build_cache();

    // O(1) lookup in cache
    {
        std::lock_guard<std::mutex> lock(models_cache_mutex_);
        auto alias_it = public_model_aliases_.find(model_name);
        std::string canonical_name = alias_it != public_model_aliases_.end() ? alias_it->second : model_name;
        if (models_cache_.find(canonical_name) != models_cache_.end()) {
            return true;
        }
    }

    if (refresh_user_models_from_disk_for_lookup(model_name)) {
        std::lock_guard<std::mutex> lock(models_cache_mutex_);
        auto alias_it = public_model_aliases_.find(model_name);
        std::string canonical_name = alias_it != public_model_aliases_.end() ? alias_it->second : model_name;
        return models_cache_.find(canonical_name) != models_cache_.end();
    }

    return false;
}

std::optional<std::string> ModelManager::validate_collection_request(
    const std::string& model_name, const json& model_data) {
    // A registry-backed collection is registered as a pointer: recipe + a checkpoint
    // that names the HF repo, with no inline components. /pull downloads the
    // repo's manifest to disk and resolves the components from it, so there is
    // nothing to validate here — accept the pointer body.
    std::string checkpoint_pointer = model_data.value("checkpoint", std::string());
    if (checkpoint_pointer.empty() && model_data.contains("checkpoints") &&
        model_data["checkpoints"].is_object()) {
        checkpoint_pointer = model_data["checkpoints"].value("main", std::string());
    }
    const bool has_components =
        model_data.contains("components") && model_data["components"].is_array() &&
        !model_data["components"].empty();
    if (!has_components) {
        if (!checkpoint_pointer.empty()) {
            return std::nullopt;  // pointer-only registry-backed collection
        }
        return std::string("Collection recipe requires a non-empty 'components' array");
    }
    // An inline collection import carries its component definitions in a `models`
    // array. Every component must be *resolvable*: either it is already
    // registered locally (local-wins) or it has a matching definition in
    // `models`. Validate this per-component and fail closed. `models` being
    // present is not a blanket license — a component it omits would otherwise
    // pass here and then be silently skipped during registration, persisting a
    // collection smaller than the imported file describes. Mirror the
    // name-matching that register_components() uses (bare names; `model_name`
    // or `id` keys) so this gate rejects exactly what registration would drop.
    const bool has_models_array =
        model_data.contains("models") && model_data["models"].is_array();
    auto inline_def_name = [](const json& def) -> std::string {
        if (!def.is_object()) return "";
        for (const char* key : {"model_name", "id"}) {
            if (def.contains(key) && def[key].is_string()) {
                return bare_component_name(def[key].get<std::string>());
            }
        }
        return "";
    };
    auto find_inline_def = [&](const std::string& bare) -> const json* {
        if (!has_models_array) return nullptr;
        for (const auto& candidate : model_data["models"]) {
            if (inline_def_name(candidate) == bare) return &candidate;
        }
        return nullptr;
    };
    const std::string bare_collection = bare_component_name(model_name);
    for (const auto& component : model_data["components"]) {
        if (!component.is_string()) {
            return std::string("components entries must be strings");
        }
        std::string component_name = component.get<std::string>();
        std::string bare = bare_component_name(component_name);
        // Self-reference: compare bare forms so a collection `user.MyCol` with
        // `components: ["MyCol"]` is rejected too, not only the exact-string case.
        if (bare == bare_collection) {
            return "Collection cannot reference itself: " + component_name;
        }
        // Components must be regular models, not collections.
        const json* def = find_inline_def(bare);
        bool is_collection_component =
            (def != nullptr && def->is_object() &&
             is_model_collection_recipe(def->value("recipe", std::string()))) ||
            (model_exists(bare) && is_model_collection_recipe(get_model_info(bare).recipe));
        if (is_collection_component) {
            return "Collection components must be regular models, not collections: '" +
                   component_name + "'";
        }
        if (has_models_array) {
            if (!model_exists(bare) && def == nullptr) {
                return "Inline collection component '" + component_name +
                       "' has no matching definition in 'models' and is not a "
                       "registered model. Every component must be defined inline "
                       "(matching 'model_name') or reference an already-registered "
                       "model.";
            }
            // A component that resolves to its inline definition (rather than an
            // already-registered model) must be a usable registration. Reject a
            // half-defined component here instead of registering it and failing
            // later mid-download.
            if (!model_exists(bare) && def != nullptr &&
                !collection_component_def_is_valid(*def)) {
                return "Inline collection component '" + component_name +
                       "' has an incomplete definition in 'models' (a recipe and "
                       "at least one checkpoint are required).";
            }
        } else if (!model_exists(component_name)) {
            return "Collection component not registered: '" + component_name +
                   "'. Pull or register it before referencing it in a collection.";
        }
    }
    if (is_router_collection_recipe(model_data.value("recipe", std::string()))) {
        RoutingPolicyParseOptions options;
        options.resolve_component = [this](const std::string& name) -> std::optional<std::string> {
            try {
                return resolve_model_name(name);
            } catch (...) {
                // Inline collection imports may reference components that are
                // defined in the same JSON but not registered yet. Keep those
                // names stable so parser component-role validation can still
                // compare them against the authored components array.
                return name;
            }
        };
        try {
            RoutePolicy policy = parse_route_policy_collection(model_data, options);
            RoutingPolicyEngine(std::move(policy), ClassifierServices{});
        } catch (const std::exception& e) {
            return std::string("Invalid collection.router routing policy: ") + e.what();
        }
    }
    return std::nullopt;
}

bool ModelManager::model_exists_unfiltered(const std::string& model_name) {
    auto exists_in_registries = [this](const std::string& name) -> bool {
        // Direct match in server_models_ (built-ins are keyed bare).
        if (server_models_.contains(name)) {
            return true;
        }
        if (auto canon = parse_canonical_id(name)) {
            switch (canon->source) {
                case ModelSource::Registered:
                    return user_models_.contains(canon->bare_name);
                case ModelSource::Builtin:
                    return server_models_.contains(canon->bare_name);
                case ModelSource::Imported:
                    // extra.* models are filesystem-discovered; not in either JSON registry.
                    return false;
            }
        }
        return false;
    };

    if (exists_in_registries(model_name)) {
        return true;
    }

    std::string canonical_name = resolve_model_name(model_name);
    if (exists_in_registries(canonical_name) || server_models_.contains(canonical_name)) {
        return true;
    }

    // If a stale warm cache caused the alias/registry lookup to miss, reload the
    // persisted user registry before reporting a hard "not found".
    if (refresh_user_models_from_disk_for_lookup(model_name)) {
        if (exists_in_registries(model_name)) {
            return true;
        }
        canonical_name = resolve_model_name(model_name);
        return exists_in_registries(canonical_name) || server_models_.contains(canonical_name);
    }

    return false;
}

ModelInfo ModelManager::get_model_info_unfiltered(const std::string& model_name) {
    ModelInfo info;
    std::string registry_name = model_name;
    bool is_user_lookup = false;

    auto try_resolve = [&](const std::string& name) -> bool {
        if (server_models_.contains(name)) {
            registry_name = name;
            is_user_lookup = false;
            return true;
        }
        if (auto canon = parse_canonical_id(name)) {
            if (canon->source == ModelSource::Builtin && server_models_.contains(canon->bare_name)) {
                registry_name = canon->bare_name;
                is_user_lookup = false;
                return true;
            }
            if (canon->source == ModelSource::Registered && user_models_.contains(canon->bare_name)) {
                registry_name = canon->bare_name;
                is_user_lookup = true;
                return true;
            }
        }
        return false;
    };

    bool resolved = try_resolve(model_name);
    if (!resolved) {
        std::string canonical_name = resolve_model_name(model_name);
        if (canonical_name != model_name) {
            resolved = try_resolve(canonical_name);
        }
    }

    if (!resolved && refresh_user_models_from_disk_for_lookup(model_name)) {
        resolved = try_resolve(model_name);
        if (!resolved) {
            std::string canonical_name = resolve_model_name(model_name);
            if (canonical_name != model_name) {
                resolved = try_resolve(canonical_name);
            }
        }
    }

    json* model_json = nullptr;
    if (is_user_lookup && user_models_.contains(registry_name)) {
        model_json = &user_models_[registry_name];
    } else if (!is_user_lookup && server_models_.contains(registry_name)) {
        model_json = &server_models_[registry_name];
    }

    if (!model_json) {
        throw std::runtime_error("Model not found in registry: " + model_name);
    }

    info.model_name = is_user_lookup
        ? canonical_id(ModelSource::Registered, registry_name)
        : registry_name;
    info.checkpoints["main"] = JsonUtils::get_or_default<std::string>(*model_json, "checkpoint", "");
    parse_legacy_mmproj(info, *model_json);
    load_checkpoints(info, *model_json);
    parse_components(info, *model_json);
    info.recipe = JsonUtils::get_or_default<std::string>(*model_json, "recipe", "");
    info.suggested = JsonUtils::get_or_default<bool>(*model_json, "suggested", false);
    parse_model_source_fields(info, *model_json);
    info.system_prompt = JsonUtils::get_or_default<std::string>(*model_json, "system_prompt", "");

    // Parse labels array
    if (model_json->contains("labels") && (*model_json)["labels"].is_array()) {
        for (const auto& label : (*model_json)["labels"]) {
            if (label.is_string()) {
                info.labels.push_back(label.get<std::string>());
            }
        }
    }

    // Parse size
    if (model_json->contains("size")) {
        if ((*model_json)["size"].is_number()) {
            info.size = (*model_json)["size"].get<double>();
        }
    }

    parse_extras(info, *model_json);

    return info;
}

std::string ModelManager::get_model_filter_reason(const std::string& model_name) {
    // Ensure cache is built (this populates filtered_out_models_)
    build_cache();

    // Look up in the filtered-out models cache
    // This is populated by filter_models_by_backend() during cache building
    std::lock_guard<std::mutex> lock(models_cache_mutex_);

    auto alias_it = public_model_aliases_.find(model_name);
    std::string canonical_name = alias_it != public_model_aliases_.end() ? alias_it->second : model_name;
    auto it = filtered_out_models_.find(canonical_name);
    if (it != filtered_out_models_.end()) {
        return it->second;
    }

    // Model wasn't filtered out (either it's available or doesn't exist)
    return "";
}

// Must be called with models_cache_mutex_ held.
//
// Populates two maps that drive the friendly-name / canonical-ID system:
//
//   public_model_aliases_ — input alias → cache key (canonical name in cache):
//     - <bare> → cache key of the precedence-winner for that bare name
//     - builtin.<X> → bare cache key X (built-ins are keyed bare in the cache)
//     - user.<X>, extra.<X> resolve directly via cache lookup fallback in the
//       callers, so no identity entries are required here
//
//   canonical_public_names_ — cache key → wire-format ID emitted by the API:
//     - winner cache keys → bare name
//     - shadowed cache keys → canonical-prefixed ID (user.X / extra.X / builtin.X)
//
// Precedence: Registered > Imported > Builtin.
void ModelManager::rebuild_public_model_aliases_locked() {
    public_model_aliases_.clear();
    canonical_public_names_.clear();

    struct Entry {
        std::string cache_key;
        ModelSource source;
    };
    std::map<std::string, std::vector<Entry>> by_bare;

    for (const auto& [cache_key, info] : models_cache_) {
        ModelSource source;
        std::string bare;
        if (auto canon = parse_canonical_id(cache_key)) {
            source = canon->source;
            bare = canon->bare_name;
        } else {
            // Unprefixed cache keys are built-ins (server_models.json).
            source = ModelSource::Builtin;
            bare = cache_key;
        }
        by_bare[bare].push_back({cache_key, source});
    }

    for (auto& [bare, entries] : by_bare) {
        std::sort(entries.begin(), entries.end(),
                  [](const Entry& a, const Entry& b) {
                      return precedence_rank(a.source) < precedence_rank(b.source);
                  });

        const Entry& winner = entries.front();
        public_model_aliases_[bare] = winner.cache_key;
        canonical_public_names_[winner.cache_key] = bare;

        for (size_t i = 1; i < entries.size(); ++i) {
            const Entry& shadowed = entries[i];
            std::string canonical = canonical_id(shadowed.source, bare);
            canonical_public_names_[shadowed.cache_key] = canonical;
            if (canonical != shadowed.cache_key) {
                public_model_aliases_[canonical] = shadowed.cache_key;
            }
        }
    }

    // Always accept builtin.<X> as an input alias for the bare cache key,
    // even when no other source shadows the built-in.
    for (const auto& [cache_key, _info] : models_cache_) {
        if (parse_canonical_id(cache_key)) continue;
        std::string canonical = canonical_id(ModelSource::Builtin, cache_key);
        public_model_aliases_.try_emplace(canonical, cache_key);
    }
}

} // namespace lemon

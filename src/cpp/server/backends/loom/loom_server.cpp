#include "lemon/backends/loom/loom_server.h"

#include "lemon/backend_manager.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/hf_cache_util.h"
#include "lemon/backends/loom/loom.h"
#include "lemon/backends/loom/loom_bundle.h"
#include "lemon/model_manager.h"
#include "lemon/models/artifact_ref.h"
#include "lemon/models/fetchable_repository.h"
#include "lemon/models/registry_id.h"
#include "lemon/models/registry_types.h"
#include "lemon/server_process.h"
#include "lemon/system_info.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/process_manager.h"

#include <lemon/utils/aixlog.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace lemon {
namespace backends {

namespace {

constexpr const char* kBackend = "rocm";
constexpr long kBundleTimeoutSeconds = 3600;

fs::path store_root() {
    return utils::path_from_utf8(utils::get_hf_cache_dir()) / "loom";
}

fs::path run_dir(const std::string& checkpoint) {
    return store_root() / loom::store_dir_name(models::repo_id_from_checkpoint(checkpoint));
}

json read_json_file(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("Cannot read " + utils::path_to_utf8(path));
    return json::parse(stream);
}

bool bundle_complete(const std::string& checkpoint) {
    std::error_code ec;
    const fs::path dir = run_dir(checkpoint);
    return fs::is_regular_file(dir / loom::kCompleteMarker, ec) &&
           fs::is_regular_file(dir / loom::kCardFile, ec);
}

bool file_matches(const fs::path& path, std::uint64_t size, const std::string& sha256) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || fs::file_size(path, ec) != size || ec) return false;
    std::string error;
    return utils::HttpClient::verify_file_digest(utils::path_to_utf8(path), "sha256", sha256, error);
}

// Pulls a bundle repository and the origin files its lock pins into one run
// folder, reusing any copy the models directory already holds.
class BundleFetcher {
public:
    BundleFetcher(std::string repo_id, const models::RegistryId& source,
                  DownloadProgressCallback progress, utils::CancelToken cancel)
        : repo_id_(std::move(repo_id)), source_(source), progress_(std::move(progress)),
          cancel_(std::move(cancel)) {}

    void run() {
        if (source_ != models::huggingface_descriptor().id) {
            throw std::runtime_error("Loom bundles download from Hugging Face only");
        }
        models::ArtifactRef ref;
        ref.registry = source_;
        ref.checkpoint_type = "main";
        ref.repo_id = repo_id_;
        repository_ = models::create_repository(source_, ref);
        if (!repository_) throw std::runtime_error("No Hugging Face repository adapter");

        const models::RepoListing listing = repository_->list(repo_id_);
        dir_ = run_dir(repo_id_);
        std::error_code ec;
        fs::create_directories(dir_, ec);
        fs::remove(dir_ / loom::kCompleteMarker, ec);

        std::vector<RegistryFile> bundle_files;
        for (const auto& file : listing.files) {
            if (file.directory || file.path == ".gitattributes") continue;
            if (!loom::is_contained_relative_path(file.path)) {
                throw std::runtime_error("Loom bundle file escapes its folder: " + file.path);
            }
            bundle_files.push_back(file);
        }

        const auto fetch_listed = [&](const std::string& path) {
            for (const auto& file : bundle_files) {
                if (file.path == path) return fetch_bundle_file(file, listing.revision);
            }
            throw std::runtime_error(repo_id_ + " is not a Loom bundle: it has no " + path);
        };
        fetch_listed(loom::kCardFile);
        fetch_listed(loom::kLockFile);
        const loom::BundlePlan plan =
            loom::plan_bundle(read_json_file(dir_ / loom::kCardFile),
                              read_json_file(dir_ / loom::kLockFile));

        total_files_ = static_cast<int>(bundle_files.size() + plan.origin_files.size());
        for (const auto& file : bundle_files) total_bytes_ += file.size;
        for (const auto& file : plan.origin_files) total_bytes_ += file.size;

        for (const auto& file : bundle_files) fetch_bundle_file(file, listing.revision);
        for (const auto& file : plan.origin_files) fetch_origin_file(file);
        for (const auto& file : plan.locked_files) {
            if (!file_matches(dir_ / utils::path_from_utf8(file.path), file.size, file.sha256)) {
                throw std::runtime_error(file.path + " does not match the bundle's lock");
            }
        }

        std::ofstream marker(dir_ / loom::kCompleteMarker, std::ios::binary | std::ios::trunc);
        marker << json{{"repo_id", repo_id_}, {"revision", listing.revision.id},
                       {"model_id", plan.model_id}}.dump(2);
        if (!marker.good()) throw std::runtime_error("Cannot record the completed Loom bundle");
        marker.close();

        if (progress_) {
            DownloadProgress done;
            done.complete = true;
            done.file_index = total_files_;
            done.total_files = total_files_;
            done.total_download_size = static_cast<size_t>(total_bytes_);
            done.percent = 100;
            (void)progress_(done);
        }
    }

private:
    bool fetch_bundle_file(const RegistryFile& file, const models::Revision& revision) {
        const fs::path target = dir_ / utils::path_from_utf8(file.path);
        if (fetched_.count(file.path)) return true;
        fetched_.insert(file.path);
        std::string error;
        std::error_code ec;
        const bool present = fs::is_regular_file(target, ec) && fs::file_size(target, ec) == file.size &&
                             (file.hash.empty() ||
                              utils::HttpClient::verify_file_digest(utils::path_to_utf8(target),
                                                                     file.hash_algorithm, file.hash,
                                                                     error));
        if (present) {
            report_skipped(file.path, file.size);
            return true;
        }
        fs::remove(target, ec);
        download(repository_->file_url({repo_id_, revision, file.path}), target, file.path,
                 file.size, file.hash_algorithm, file.hash);
        return true;
    }

    void fetch_origin_file(const loom::OriginFile& file) {
        const fs::path target = dir_ / utils::path_from_utf8(file.path);
        if (file_matches(target, file.size, file.sha256)) {
            report_skipped(file.path, file.size);
            return;
        }
        std::error_code ec;
        fs::remove(target, ec);
        fs::create_directories(target.parent_path(), ec);
        if (adopt_cached_copy(file, target)) {
            report_skipped(file.path, file.size);
            return;
        }
        download(repository_->file_url({file.repository, models::Revision{file.revision, true},
                                        file.file}),
                 target, file.path, file.size, "sha256", file.sha256);
    }

    bool adopt_cached_copy(const loom::OriginFile& file, const fs::path& target) {
        const fs::path cache = utils::path_from_utf8(utils::get_hf_cache_dir()) /
                               registry_repo_cache_dir_name(file.repository, source_);
        std::vector<fs::path> candidates = {
            cache / "snapshots" / file.revision / utils::path_from_utf8(file.file),
            cache / "blobs" / file.sha256,
        };
        const fs::path active = hf_cache::active_snapshot_path(cache);
        if (!active.empty()) candidates.push_back(active / utils::path_from_utf8(file.file));

        for (const auto& candidate : candidates) {
            std::error_code ec;
            const fs::path resolved = fs::canonical(candidate, ec);
            if (ec || !file_matches(resolved, file.size, file.sha256)) continue;
            fs::create_hard_link(resolved, target, ec);
            if (ec) {
                ec.clear();
                fs::copy_file(resolved, target, fs::copy_options::overwrite_existing, ec);
            }
            if (!ec) {
                LOG(INFO, "Loom") << "Reusing " << utils::path_to_utf8(resolved) << " for "
                                  << file.path << std::endl;
                return true;
            }
        }
        return false;
    }

    void download(const std::string& url, const fs::path& target, const std::string& name,
                  std::uint64_t size, const std::string& hash_algorithm, const std::string& hash) {
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        ++file_index_;
        utils::DownloadOptions options;
        options.max_retries = 10;
        options.initial_retry_delay_ms = 2000;
        options.max_retry_delay_ms = 120000;
        options.connect_timeout = 60;
        options.low_speed_limit = 1000;
        options.low_speed_time = static_cast<int>(utils::HttpClient::get_default_timeout());
        options.expected_hash_algorithm = hash_algorithm;
        options.expected_hash = hash;

        const auto on_bytes = [&](size_t downloaded, size_t total) -> bool {
            if (cancel_.cancelled()) return false;
            if (!progress_) return true;
            DownloadProgress update;
            update.file = name;
            update.file_index = file_index_;
            update.total_files = total_files_;
            update.bytes_downloaded = downloaded;
            update.bytes_total = total > 0 ? total : static_cast<size_t>(size);
            update.total_download_size = static_cast<size_t>(total_bytes_);
            update.percent = update.bytes_total > 0
                                 ? static_cast<int>((downloaded * 100) / update.bytes_total)
                                 : 0;
            return progress_(update);
        };

        LOG(INFO, "Loom") << "Downloading " << name << " (" << size << " bytes)" << std::endl;
        const auto result =
            utils::HttpClient::download_file(url, utils::path_to_utf8(target), on_bytes,
                                             repository_->auth_headers(), options,
                                             utils::HttpSecurityPolicy::ExternalHttpsOnly, cancel_);
        if (result.cancelled) throw std::runtime_error("Download cancelled");
        if (!result.success) {
            throw std::runtime_error("Failed to download " + name + ": " + result.error_message);
        }
    }

    void report_skipped(const std::string& name, std::uint64_t size) {
        ++file_index_;
        if (!progress_) return;
        DownloadProgress update;
        update.file = name;
        update.file_index = file_index_;
        update.total_files = total_files_;
        update.bytes_downloaded = static_cast<size_t>(size);
        update.bytes_total = static_cast<size_t>(size);
        update.total_download_size = static_cast<size_t>(total_bytes_);
        update.bytes_previously_downloaded = static_cast<size_t>(size);
        update.percent = 100;
        (void)progress_(update);
    }

    std::string repo_id_;
    models::RegistryId source_;
    DownloadProgressCallback progress_;
    utils::CancelToken cancel_;
    std::unique_ptr<models::FetchableRepository> repository_;
    fs::path dir_;
    std::set<std::string> fetched_;
    int file_index_ = 0;
    int total_files_ = 0;
    std::uint64_t total_bytes_ = 0;
};

void require_strix_halo() {
    std::string arch;
    try {
        const GPUInfo igpu = create_system_info()->get_amd_igpu_device();
        if (igpu.available) arch = identify_rocm_arch_from_name(igpu.name);
    } catch (...) {
    }
    if (arch != "gfx1151") {
        throw std::runtime_error("Loom requires an AMD Strix Halo GPU (gfx1151)" +
                                 (arch.empty() ? std::string() : ", found " + arch));
    }
}

// The server process does not keep its console output, so a failed start is
// re-run once with captured output. Startup failures (device, card) are
// deterministic and happen before loom-serve listens, so the re-run reproduces
// them within seconds; a re-run that keeps serving is cut off by the timeout.
std::string diagnose_startup_failure(const ServerCommand& command) {
    constexpr int kProbeSeconds = 15;
    std::vector<std::string> lines;
    try {
        utils::ProcessManager::run_process_with_output(
            command.program, command.args,
            [&lines](const std::string& line) {
                lines.push_back(line);
                return true;
            },
            "", kProbeSeconds);
    } catch (const std::exception& error) {
        LOG(WARNING, "Loom") << "Could not re-run loom-serve to diagnose its startup failure: "
                             << error.what() << std::endl;
        return "";
    }
    return loom::startup_failure_detail(lines);
}

json get_loopback_json(int port, const std::string& path) {
    const auto response = utils::HttpClient::get("http://127.0.0.1:" + std::to_string(port) + path,
                                                 {}, 5, utils::HttpSecurityPolicy::TrustedLoopback);
    if (response.status_code != 200) return json();
    return json::parse(response.body, nullptr, false);
}

}  // namespace

LoomServer::LoomServer(const std::string& log_level, ModelManager* model_manager,
                       BackendManager* backend_manager)
    : WrappedServer("loom-serve", log_level, model_manager, backend_manager) {}

InstallParams LoomServer::get_install_params(const std::string& backend, const std::string& version) {
    if (backend != kBackend) {
        throw std::runtime_error("Loom has no '" + backend + "' backend");
    }
    const std::string wheel_version = version.rfind('v', 0) == 0 ? version.substr(1) : version;
    InstallParams params;
    params.repo = "ROCm/hrx-system";
#if defined(_WIN32)
    params.filename = "hrx_loom-" + wheel_version + "-py3-none-win_amd64.whl";
#elif defined(__linux__) && (defined(__x86_64__) || defined(__amd64__))
    params.filename = "hrx_loom-" + wheel_version + "-py3-none-manylinux_2_39_x86_64.whl";
#else
    throw std::runtime_error("Loom is available on Windows and Linux x86-64 only");
#endif
    params.archive_subdir = "hrx_loom-" + wheel_version + ".data/scripts";
    return params;
}

void LoomServer::load(const std::string& model_name, const ModelInfo& model_info,
                      const RecipeOptions& options, bool do_not_upgrade) {
    (void)do_not_upgrade;
    require_strix_halo();
    device_type_ = DEVICE_GPU;
    backend_manager_->install_backend(loom::descriptor.recipe, kBackend);

    const std::string checkpoint = model_info.checkpoint();
    if (!bundle_complete(checkpoint)) {
        BundleFetcher(models::repo_id_from_checkpoint(checkpoint),
                      model_manager_->download_source_for(model_info), nullptr, {})
            .run();
    }
    const fs::path dir = run_dir(checkpoint);
    const loom::BundlePlan plan = loom::plan_bundle(read_json_file(dir / loom::kCardFile),
                                                    read_json_file(dir / loom::kLockFile));
    served_model_id_ = plan.model_id;

    std::string memory_bytes = options.get_option("loom_memory_bytes").get<std::string>();
    if (memory_bytes.empty() && fs::exists(dir / loom::kInfoFile)) {
        memory_bytes = loom::card_memory_bytes(read_json_file(dir / loom::kInfoFile));
    }

    port_ = choose_port();
    ServerCommand command;
    command.program = BackendUtils::get_backend_binary_path(*loom::spec(), kBackend);
    command.args = loom::build_server_argv(utils::path_to_utf8(dir), port_, plan.preset,
                                           options.get_option("loom_device").get<std::string>(),
                                           memory_bytes,
                                           options.get_option("loom_args").get<std::string>());
    command.port = port_;
    command.ready_endpoint = "/readyz";

    LOG(INFO, "Loom") << "Starting loom-serve for " << model_name << " (" << served_model_id_
                      << ") on port " << port_ << std::endl;
    const bool inherit_output = (log_level_ == "info") || is_debug();
    try {
        start_server(std::make_unique<NativeProcess>(ProcessOutput{inherit_output, false}), command);
    } catch (const std::runtime_error&) {
        if (load_cancelled()) throw;
        const std::string detail = diagnose_startup_failure(command);
        if (detail.empty()) throw;
        throw std::runtime_error("loom-serve failed to start: " + detail);
    }
    wait_for_bundle(model_name);
}

void LoomServer::wait_for_bundle(const std::string& model_name) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(kBundleTimeoutSeconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (load_cancelled()) {
            stop_server();
            throw std::runtime_error("Load of " + model_name + " was cancelled");
        }
        if (!is_backend_alive()) {
            stop_server();
            throw std::runtime_error("loom-serve exited while preparing " + model_name);
        }
        const loom::ReadinessState state = loom::parse_readiness(
            get_loopback_json(port_, "/v1/models"), get_loopback_json(port_, "/v1/status"),
            served_model_id_);
        if (state.state == loom::Readiness::Ready) {
            LOG(INFO, "Loom") << model_name << " is ready" << std::endl;
            return;
        }
        if (state.state == loom::Readiness::Failed) {
            stop_server();
            throw std::runtime_error("loom-serve could not prepare " + model_name + ": " + state.error);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    stop_server();
    throw std::runtime_error("loom-serve did not prepare " + model_name + " in time");
}

json LoomServer::chat_completion(const json& request) {
    return forward_request("/v1/chat/completions", loom::prepare_chat_request(request, served_model_id_));
}

void LoomServer::forward_streaming_request(const std::string& endpoint,
                                           const std::string& request_body,
                                           httplib::DataSink& sink, bool sse,
                                           long timeout_seconds,
                                           TelemetryCallback telemetry_callback) {
    std::string body = request_body;
    if (endpoint.find("chat/completions") != std::string::npos) {
        const json parsed = json::parse(request_body, nullptr, false);
        if (!parsed.is_discarded()) body = loom::prepare_chat_request(parsed, served_model_id_).dump();
    }
    WrappedServer::forward_streaming_request(endpoint, body, sink, sse, timeout_seconds,
                                             std::move(telemetry_callback));
}

namespace loom {

namespace {

class LoomOps : public BackendOps {
public:
    std::string resolve_checkpoint_path(const ModelInfo& info,
                                        const CheckpointResolveContext& ctx) const override {
        (void)info;
        try {
            return utils::path_to_utf8(run_dir(ctx.checkpoint));
        } catch (const std::invalid_argument&) {
            return "";
        }
    }

    bool is_downloaded(const ModelInfo& info, const BackendOpsContext&) const override {
        try {
            return bundle_complete(info.checkpoint());
        } catch (const std::invalid_argument&) {
            return false;
        }
    }

    void download_model(const ModelInfo& info, bool do_not_upgrade,
                        DownloadProgressCallback progress,
                        const BackendOpsContext& ctx) const override {
        if (do_not_upgrade && is_downloaded(info, ctx)) return;
        if (ctx.fetch == nullptr) {
            throw std::runtime_error("No fetch service while resolving the download source for: " +
                                     info.checkpoint());
        }
        BundleFetcher(models::repo_id_from_checkpoint(info.checkpoint()),
                      ctx.fetch->download_source_for(info), std::move(progress), ctx.cancel)
            .run();
    }

    void remove_self_managed_model(const ModelInfo& info) const override {
        std::error_code ec;
        fs::remove_all(run_dir(info.checkpoint()), ec);
        if (ec) {
            throw std::runtime_error("Failed to remove the Loom bundle for " + info.checkpoint() +
                                     ": " + ec.message());
        }
    }

    bool invalidates_cache_after_download() const override { return true; }

    std::string validate_registration_checkpoint(const std::string& checkpoint) const override {
        if (checkpoint.find(':') != std::string::npos) {
            return "Loom checkpoints name a whole bundle repository (org/name), without a variant";
        }
        try {
            (void)store_dir_name(checkpoint);
        } catch (const std::invalid_argument& error) {
            return error.what();
        }
        return "";
    }

    void populate_metadata(ModelInfo& info, const BackendOpsContext&) const override {
        try {
            const fs::path dir = run_dir(info.checkpoint());
            std::error_code ec;
            if (!fs::is_regular_file(dir / kCardFile, ec)) return;
            const json card = read_json_file(dir / kCardFile);
            const json presets = card.value("presets", json::object());
            for (const auto& [name, preset] : presets.items()) {
                if (!preset.is_object()) continue;
                const json execution = preset.value("execution", json::object());
                if (!execution.is_object()) continue;
                if (execution.value("context_capacity", 0) > 0) {
                    info.max_context_window = execution["context_capacity"].get<int64_t>();
                }
                if (execution.value("max_tokens", 0) > 0) {
                    info.max_output_tokens = execution["max_tokens"].get<int64_t>();
                }
                if (name == "chat") break;
            }
        } catch (const std::exception& error) {
            LOG(WARNING, "Loom") << "Cannot read the card for " << info.checkpoint() << ": "
                                 << error.what() << std::endl;
        }
    }
};

}  // namespace

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) {
    return make_server<LoomServer>(ctx);
}

const BackendSpec* spec() { return make_spec<LoomServer>(descriptor); }

const BackendOps* ops() { return single_ops<LoomOps>(); }

}  // namespace loom

}  // namespace backends
}  // namespace lemon

#include "lemon/backends/loom/loom_server.h"

#include "lemon/backend_manager.h"
#include "lemon/backends/backend_ops.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/backends/hf_cache_util.h"
#include "lemon/backends/loom/loom.h"
#include "lemon/backends/loom/loom_bundle.h"
#include "lemon/model_manager.h"
#include "lemon/models/artifact_ref.h"
#include "lemon/models/artifact_resolver.h"
#include "lemon/models/artifact_transfer.h"
#include "lemon/models/fetchable_repository.h"
#include "lemon/models/registry_id.h"
#include "lemon/models/registry_types.h"
#include "lemon/server_process.h"
#include "lemon/system_info.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/process_manager.h"

#include <lemon/utils/aixlog.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
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

// A run folder fetched for another Loom release holds a card this loom-serve
// rejects (0.2.0 refuses 0.1.0's `pool_capacity`), so it counts as missing.
bool bundle_complete(const std::string& checkpoint) {
    std::error_code ec;
    const fs::path dir = run_dir(checkpoint);
    if (!fs::is_regular_file(dir / loom::kCompleteMarker, ec) ||
        !fs::is_regular_file(dir / loom::kCardFile, ec)) {
        return false;
    }
    try {
        return read_json_file(dir / loom::kCompleteMarker).value("loom", "") ==
               loom::kBundleRevision;
    } catch (const std::exception&) {
        return false;
    }
}

bool file_matches(const fs::path& path, std::uint64_t size, const std::string& algorithm,
                  const std::string& hash) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || fs::file_size(path, ec) != size || ec) return false;
    std::string error;
    return hash.empty() ||
           utils::HttpClient::verify_file_digest(utils::path_to_utf8(path), algorithm, hash, error);
}

// The download re-hashes a file it finds in place, but takes one the listing
// gives no hash for as complete whatever its size.
void drop_if_resized(const fs::path& path, std::uint64_t size) {
    std::error_code ec;
    if (fs::is_regular_file(path, ec) && fs::file_size(path, ec) != size) fs::remove(path, ec);
}

models::ArtifactRef bundle_ref(const std::string& checkpoint, const models::RegistryId& source) {
    if (source != models::huggingface_descriptor().id) {
        throw std::runtime_error("Loom bundles download from Hugging Face only");
    }
    models::ArtifactRef ref;
    ref.registry = source;
    ref.checkpoint_type = "main";
    ref.repo_id = models::repo_id_from_checkpoint(checkpoint);
    return ref;
}

// Pulls a bundle repository and the origin files its lock pins into one run
// folder, reusing any copy the models directory already holds.
class BundleFetcher {
public:
    // A non-empty `target` refuses a bundle that does not support it before its
    // weights download.
    BundleFetcher(std::unique_ptr<models::FetchableRepository> repository,
                  DownloadProgressCallback progress, utils::CancelToken cancel,
                  std::string target = "")
        : repository_(std::move(repository)), repo_id_(repository_->ref().repo_id),
          progress_(std::move(progress)), cancel_(std::move(cancel)),
          target_(std::move(target)) {}

    void run() {
        const models::RepoListing listing = list_bundle();
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

        models::DownloadPlan card_plan{utils::path_to_utf8(dir_), {}};
        for (const char* name : {loom::kCardFile, loom::kLockFile}) {
            const auto it = std::find_if(bundle_files.begin(), bundle_files.end(),
                                         [&](const RegistryFile& file) { return file.path == name; });
            if (it == bundle_files.end()) {
                throw std::runtime_error(repo_id_ + " is not a Loom bundle: it has no " + name);
            }
            card_plan.files.push_back(bundle_download(*it, listing.revision));
        }
        const auto info_file =
            std::find_if(bundle_files.begin(), bundle_files.end(),
                         [](const RegistryFile& file) { return file.path == loom::kInfoFile; });
        if (!target_.empty() && info_file != bundle_files.end()) {
            card_plan.files.push_back(bundle_download(*info_file, listing.revision));
        }
        execute(card_plan, nullptr);
        if (!target_.empty() && info_file != bundle_files.end() &&
            !loom::bundle_supports_target(read_json_file(dir_ / loom::kInfoFile), target_)) {
            throw std::runtime_error(repo_id_ + " is not supported on this GPU (" + target_ + ")");
        }
        const loom::BundlePlan bundle =
            loom::plan_bundle(read_json_file(dir_ / loom::kCardFile),
                              read_json_file(dir_ / loom::kLockFile));

        models::DownloadPlan plan{utils::path_to_utf8(dir_), {}};
        for (const auto& file : bundle_files) plan.files.push_back(bundle_download(file, listing.revision));
        for (const auto& file : bundle.origin_files) plan.files.push_back(origin_download(file));
        execute(plan, progress_);
        for (const auto& file : bundle.locked_files) {
            if (!file_matches(dir_ / utils::path_from_utf8(file.path), file.size, "sha256",
                              file.sha256)) {
                throw std::runtime_error(file.path + " does not match the bundle's lock");
            }
        }

        std::ofstream marker(dir_ / loom::kCompleteMarker, std::ios::binary | std::ios::trunc);
        marker << json{{"repo_id", repo_id_}, {"revision", listing.revision.id},
                       {"model_id", bundle.model_id}, {"loom", loom::kBundleRevision}}.dump(2);
        if (!marker.good()) throw std::runtime_error("Cannot record the completed Loom bundle");
        marker.close();

        if (progress_) {
            DownloadProgress done;
            done.complete = true;
            done.percent = 100;
            (void)progress_(done);
        }
    }

private:
    // loom-fun tags a bundle with a Loom release only once its default branch
    // moves on to a newer one, so a missing tag means the head still serves ours.
    models::RepoListing list_bundle() const {
        try {
            return repository_->list(repo_id_, loom::kBundleRevision);
        } catch (const RegistryNotFoundError&) {
            LOG(INFO, "Loom") << repo_id_ << " has no " << loom::kBundleRevision
                              << " tag; using its default branch" << std::endl;
            return repository_->list(repo_id_);
        }
    }

    models::DownloadFile bundle_download(const RegistryFile& file, const models::Revision& revision) {
        drop_if_resized(dir_ / utils::path_from_utf8(file.path), file.size);
        return {file.path, repository_->file_url({repo_id_, revision, file.path}), file.size,
                file.hash_algorithm, file.hash, ""};
    }

    models::DownloadFile origin_download(const loom::OriginFile& file) {
        const fs::path target = dir_ / utils::path_from_utf8(file.path);
        drop_if_resized(target, file.size);
        std::error_code ec;
        if (!fs::exists(target, ec)) adopt_cached_copy(file, target);
        return {file.path,
                repository_->file_url({file.repository, models::Revision{file.revision, true}, file.file}),
                file.size, "sha256", file.sha256, ""};
    }

    void adopt_cached_copy(const loom::OriginFile& file, const fs::path& target) {
        const fs::path cache = utils::path_from_utf8(utils::get_hf_cache_dir()) /
                               registry_repo_cache_dir_name(file.repository, repository_->id());
        std::vector<fs::path> candidates = {
            cache / "snapshots" / file.revision / utils::path_from_utf8(file.file),
            cache / "blobs" / file.sha256,
        };
        const fs::path active = hf_cache::active_snapshot_path(cache);
        if (!active.empty()) candidates.push_back(active / utils::path_from_utf8(file.file));

        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        for (const auto& candidate : candidates) {
            const fs::path resolved = fs::canonical(candidate, ec);
            if (ec || !file_matches(resolved, file.size, "sha256", file.sha256)) continue;
            fs::create_hard_link(resolved, target, ec);
            if (ec) {
                ec.clear();
                fs::copy_file(resolved, target, fs::copy_options::overwrite_existing, ec);
            }
            if (!ec) {
                LOG(INFO, "Loom") << "Reusing " << utils::path_to_utf8(resolved) << " for "
                                  << file.path << std::endl;
                return;
            }
        }
    }

    void execute(const models::DownloadPlan& plan, DownloadProgressCallback progress) {
        models::ArtifactTransfer::with_default_transfers().execute(
            plan, repository_->auth_headers(), std::move(progress), cancel_);
    }

    std::unique_ptr<models::FetchableRepository> repository_;
    std::string repo_id_;
    DownloadProgressCallback progress_;
    utils::CancelToken cancel_;
    std::string target_;
    fs::path dir_;
};

// The iGPU is checked first so a Strix Halo with an attached dGPU keeps the
// target it had before the R9700 was supported.
std::string require_supported_gpu() {
    std::vector<std::string> found;
    try {
        const auto system_info = create_system_info();
        std::vector<GPUInfo> gpus = {system_info->get_amd_igpu_device()};
        const auto dgpus = system_info->get_amd_dgpu_devices();
        gpus.insert(gpus.end(), dgpus.begin(), dgpus.end());
        for (const auto& gpu : gpus) {
            if (!gpu.available) continue;
            const std::string arch = identify_rocm_arch_from_name(gpu.name);
            const std::string target = loom::device_target(arch, gpu.name, gpu.vram_gb);
            if (!target.empty()) return target;
            if (!arch.empty()) found.push_back(arch);
        }
    } catch (...) {
    }
    std::string detail;
    for (const auto& arch : found) detail += (detail.empty() ? ", found " : ", ") + arch;
    throw std::runtime_error(
        "Loom requires an AMD Strix Halo (gfx1151), Radeon gfx1201 32 GB (AI PRO R9700) or "
        "Radeon gfx1201 16 GB (RX 9070 XT, RX 9070) GPU" + detail);
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
    const std::string target = require_supported_gpu();
    device_type_ = DEVICE_GPU;
    backend_manager_->install_backend(loom::descriptor.recipe, kBackend);

    const std::string checkpoint = model_info.checkpoint();
    if (!bundle_complete(checkpoint)) {
        const models::ArtifactRef ref =
            bundle_ref(checkpoint, model_manager_->download_source_for(model_info));
        BundleFetcher(model_manager_->resolver().repository_for(ref, {}), nullptr, {}, target).run();
    }
    const fs::path dir = run_dir(checkpoint);
    const json card = read_json_file(dir / loom::kCardFile);
    const loom::BundlePlan plan = loom::plan_bundle(card, read_json_file(dir / loom::kLockFile));
    served_model_id_ = plan.model_id;

    const json info = fs::exists(dir / loom::kInfoFile) ? read_json_file(dir / loom::kInfoFile) : json();
    if (!loom::bundle_supports_target(info, target)) {
        throw std::runtime_error(model_name + " is not supported on this GPU (" + target + ")");
    }

#if defined(__linux__)
    constexpr bool kPreload = true;  // ROCm/hrx-system#3332 is the Linux memlock limit
#else
    constexpr bool kPreload = false;
#endif
    const std::vector<std::string> serve_args = loom::serve_args(
        target, info, loom::parameter_bytes(plan),
        options.get_option("loom_memory_bytes").get<std::string>(), kPreload);

    port_ = choose_port();
    ServerCommand command;
    command.program = BackendUtils::get_backend_binary_path(*loom::spec(), kBackend);
    command.args = loom::build_server_argv(utils::path_to_utf8(dir), port_,
                                           loom::target_preset(card, plan.preset, target),
                                           options.get_option("loom_device").get<std::string>(),
                                           serve_args,
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
        const models::ArtifactRef ref =
            bundle_ref(info.checkpoint(), ctx.fetch->download_source_for(info));
        BundleFetcher(ctx.fetch->repository_for(ref), std::move(progress), ctx.cancel).run();
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

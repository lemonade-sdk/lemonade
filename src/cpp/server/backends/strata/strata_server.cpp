#include "lemon/backends/strata/strata_server.h"

#include "lemon/backend_manager.h"
#include "lemon/backends/install_staging.h"
#include "lemon/backends/strata/strata.h"
#include "lemon/backends/strata/strata_model.h"
#include "lemon/backends/strata/strata_process.h"
#include "lemon/backends/strata/strata_runtime.h"
#include "lemon/error_types.h"
#include "lemon/gguf_reader.h"
#include "lemon/gpu_memory_selection.h"
#include "lemon/models/fetchable_repository.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace lemon::backends {
namespace {

std::mutex model_install_mutex;

void cpu_capacity(strata::MemoryCapacity& capacity) {
#ifdef _WIN32
    DWORD size = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &size) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        LOG(INFO, "Strata") << "CPU topology unavailable; keeping engine worker defaults." << std::endl;
        return;
    }
    std::vector<unsigned char> storage(size);
    auto* records = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(storage.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, records, &size)) {
        LOG(WARNING, "Strata") << "Unable to query CPU topology; keeping engine worker defaults." << std::endl;
        return;
    }
    std::map<unsigned char, int> classes;
    for (size_t offset = 0; offset < size;) {
        const auto* record = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(storage.data() + offset);
        if (size - offset < sizeof(DWORD) * 2 || record->Size < sizeof(DWORD) * 2 ||
            record->Size > size - offset) {
            LOG(WARNING, "Strata") << "Invalid CPU topology; keeping engine worker defaults." << std::endl;
            return;
        }
        if (record->Relationship == RelationProcessorCore) ++classes[record->Processor.EfficiencyClass];
        offset += record->Size;
    }
    if (classes.size() == 2) {
        capacity.performance_cores = classes.rbegin()->second;
        capacity.efficiency_cores = classes.begin()->second;
    }
#elif defined(__linux__)
    std::map<int, std::set<std::pair<int, int>>> classes;
    std::error_code error;
    const fs::path root("/sys/devices/system/cpu");
    fs::directory_iterator entries(root, error);
    for (auto end = fs::directory_iterator(); !error && entries != end; entries.increment(error)) {
        const auto name = entries->path().filename().string();
        if (name.size() <= 3 || name.substr(0, 3) != "cpu" ||
            !std::all_of(name.begin() + 3, name.end(), [](unsigned char c) { return std::isdigit(c); })) continue;
        int online = 1;
        std::ifstream(entries->path() / "online") >> online;
        if (!online) continue;
        int type = 0, package = -1, core = -1;
        const auto topology = entries->path() / "topology";
        if (!(std::ifstream(topology / "core_type") >> type) ||
            !(std::ifstream(topology / "physical_package_id") >> package) ||
            !(std::ifstream(topology / "core_id") >> core)) {
            LOG(INFO, "Strata") << "Hybrid CPU topology unavailable; keeping engine worker defaults." << std::endl;
            return;
        }
        if (type != 1 && type != 2) {
            LOG(INFO, "Strata") << "CPU has no recognized hybrid topology; keeping engine worker defaults." << std::endl;
            return;
        }
        classes[type].emplace(package, core);
    }
    if (error) {
        LOG(WARNING, "Strata") << "Unable to read CPU topology: " << error.message() << std::endl;
    } else if (classes.size() == 2) {
        capacity.performance_cores = static_cast<int>(classes.at(2).size());
        capacity.efficiency_cores = static_cast<int>(classes.at(1).size());
    }
#endif
}

strata::MemoryCapacity memory_capacity(const std::string& backend) {
    strata::MemoryCapacity capacity;
    cpu_capacity(capacity);
#ifdef _WIN32
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        capacity.total_ram_gib = static_cast<double>(memory.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
    } else {
        LOG(WARNING, "Strata") << "Unable to query total RAM; automatic RAM tuning is disabled." << std::endl;
    }
#elif defined(__linux__)
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page_size > 0) {
        capacity.total_ram_gib = static_cast<double>(pages) * page_size / (1024.0 * 1024.0 * 1024.0);
    } else {
        LOG(WARNING, "Strata") << "Unable to query total RAM; automatic RAM tuning is disabled." << std::endl;
    }
    std::ifstream kernel("/proc/sys/kernel/osrelease");
    std::string kernel_release;
    std::getline(kernel, kernel_release);
    std::transform(kernel_release.begin(), kernel_release.end(), kernel_release.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    capacity.wsl = kernel_release.find("microsoft") != std::string::npos ||
        !utils::get_environment_variable_utf8("WSL_INTEROP").empty() ||
        !utils::get_environment_variable_utf8("WSL_DISTRO_NAME").empty();
#endif
    auto system = create_system_info();
    const bool cuda = backend == "cuda" || backend == "cuda13";
    const char* visible = std::getenv("CUDA_VISIBLE_DEVICES");
    const auto pool = select_gpu_memory_pool(cuda ? GpuMemoryVendor::Nvidia : GpuMemoryVendor::Amd,
        system->get_amd_igpu_device(), system->get_amd_dgpu_devices(),
        system->get_nvidia_gpu_devices(), system->get_apple_silicon_device(), "",
        visible ? std::optional<std::string>(visible) : std::nullopt);
    capacity.total_vram_gib = pool.total_gb;
    capacity.unified_gpu = pool.unified;
    return capacity;
}

std::string host_arch() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#else
    return "unsupported";
#endif
}

bool model_present(const strata::ModelCheckpoint& checkpoint) {
    const auto folder = checkpoint.directory();
    return checkpoint.downloads_present(folder) && strata::pack_present(folder / "pack");
}

std::unique_ptr<models::FetchableRepository> huggingface_repository() {
    auto repository = models::create_repository(models::huggingface_descriptor().id, {});
    if (!repository) throw std::runtime_error("Hugging Face repository provider is not registered");
    return repository;
}

utils::DownloadOptions registry_download_options(const models::RepoListing& repository, const std::string& file) {
    const auto entry = std::find_if(repository.files.begin(), repository.files.end(),
        [&](const auto& candidate) { return candidate.path == file && !candidate.directory; });
    if (entry == repository.files.end()) throw std::runtime_error("Strata checkpoint file is missing: " + file);
    utils::DownloadOptions options;
    if (!entry->hash.empty()) {
        options.expected_hash = entry->hash;
        options.expected_hash_algorithm = entry->hash_algorithm;
    }
    return options;
}

void prepare_download(const fs::path& path, utils::DownloadOptions& options) {
    if (options.expected_hash.empty()) {
        // The shared downloader trusts existing final files when no hash is supplied.
        fs::remove(path);
        options.resume_partial = false;
    }
}

void prepare_engine_data(const fs::path& directory, const std::string& backend,
                         const DownloadProgressCallback& progress) {
    size_t index = 0;
    const auto count = strata::engine_data_files().size();
    strata::prepare_engine_data(directory, backend, [&](const std::string& url, const fs::path& path) {
        ++index;
        utils::DownloadOptions options;
        options.resume_partial = false;
        const auto result = utils::HttpClient::download_file(url, utils::path_to_utf8(path),
            [&](size_t downloaded, size_t total) {
                DownloadProgress p;
                p.file = "Strata engine data: " + utils::path_to_utf8(path.filename());
                p.file_index = static_cast<int>(index);
                p.total_files = static_cast<int>(count);
                p.bytes_downloaded = downloaded;
                p.bytes_total = total;
                p.percent = total ? static_cast<int>((index - 1 + double(downloaded) / total) * 100 / count) : 0;
                return !utils::g_download_cancelled.load() && (!progress || progress(p));
            }, {}, options);
        if (!result.success) throw std::runtime_error("Strata engine data download failed: " + result.error_message);
        if (utils::g_download_cancelled.load()) throw std::runtime_error("Strata engine data download cancelled");
    });
}

void ensure_engine_data(const fs::path& engine_dir, const std::string& backend,
                        const DownloadProgressCallback& progress) {
    std::lock_guard<std::mutex> lock(model_install_mutex);
    const auto directory = engine_dir / "lemonade-data";
    if (strata::engine_data_present(directory, backend)) return;
    if (auto* cfg = RuntimeConfig::global(); cfg && cfg->offline()) {
        throw std::runtime_error("Strata engine data is missing or outdated; install the engine online before using offline mode");
    }
    const auto staging = engine_dir / "lemonade-data.staging";
    if (fs::exists(staging)) fs::remove_all(staging);
    try {
        prepare_engine_data(staging, backend, progress);
        if (commit_staged_install(utils::path_to_utf8(staging), utils::path_to_utf8(directory),
                                  "lemonade-data.json").empty()) {
            throw std::runtime_error("Strata engine data preparation did not produce a receipt");
        }
    } catch (...) {
        std::error_code error;
        fs::remove_all(staging, error);
        if (error) LOG(WARNING, "Strata") << "Could not remove engine data staging directory: "
            << error.message() << std::endl;
        throw;
    }
}

void prepare_draft(const strata::DraftPack& draft, const DownloadProgressCallback& progress) {
    std::lock_guard<std::mutex> lock(model_install_mutex);
    if (draft.present()) return;
    if (auto* cfg = RuntimeConfig::global(); cfg && cfg->offline()) {
        throw std::runtime_error("Strata MTP draft pack is missing; pull the model online or disable strata_mtp");
    }
    const auto folder = draft.directory();
    fs::create_directories(folder);
    const auto registry = huggingface_repository();
    const auto repository = registry->list(draft.repository, draft.revision);
    std::vector<fs::path> paths;
    int index = 0;
    const auto total_files = std::count_if(draft.files.begin(), draft.files.end(),
        [](const auto& entry) { return entry.second.source.empty(); });
    for (const auto& [name, file] : draft.files) {
        if (!file.source.empty()) continue;
        ++index;
        const auto path = folder / utils::path_from_utf8(name).filename();
        auto options = registry_download_options(repository, name);
        paths.push_back(utils::path_from_utf8(name).filename());
        if (draft.file_present(name)) continue;
        prepare_download(path, options);
        const auto result = utils::HttpClient::download_file(
            registry->file_url({draft.repository, {repository.download_revision, false}, name}),
            utils::path_to_utf8(path),
            [&](size_t downloaded, size_t total) {
                DownloadProgress p;
                p.file = "Strata MTP: " + name;
                p.file_index = index;
                p.total_files = static_cast<int>(total_files);
                p.bytes_downloaded = downloaded;
                p.bytes_total = total;
                p.percent = static_cast<int>(((index - 1) * 100 + (total ? downloaded * 100 / total : 0)) / total_files);
                return !utils::g_download_cancelled.load() && (!progress || progress(p));
            }, registry->auth_headers(), options);
        if (!result.success) throw std::runtime_error("Strata MTP download failed: " + result.error_message);
    }
    strata::record_downloads(folder, "lemonade-draft.json", draft.identity(), paths);
    if (!draft.present()) throw std::runtime_error("Strata MTP draft pack verification failed");
}

void prepare_vision(const strata::VisionPack& vision, const DownloadProgressCallback& progress) {
    std::lock_guard<std::mutex> lock(model_install_mutex);
    if (vision.present()) return;
    if (auto* cfg = RuntimeConfig::global(); cfg && cfg->offline()) {
        throw std::runtime_error("Strata vision projector is missing; pull the model online or set strata_vision=off");
    }
    fs::create_directories(vision.directory());
    const auto registry = huggingface_repository();
    const auto repository = registry->list(vision.repository, vision.revision);
    auto options = registry_download_options(repository, vision.file);
    prepare_download(vision.path(), options);
    const auto result = utils::HttpClient::download_file(
        registry->file_url({vision.repository, {repository.download_revision, false}, vision.file}),
        utils::path_to_utf8(vision.path()),
        [&](size_t downloaded, size_t total) {
            DownloadProgress p;
            p.file = "Strata vision: " + vision.file;
            p.file_index = 1; p.total_files = 1;
            p.bytes_downloaded = downloaded; p.bytes_total = total;
            p.percent = total ? static_cast<int>(downloaded * 100 / total) : 0;
            return !utils::g_download_cancelled.load() && (!progress || progress(p));
        }, registry->auth_headers(), options);
    if (!result.success) throw std::runtime_error("Strata vision download failed: " + result.error_message);
    strata::record_downloads(vision.directory(), "lemonade-vision.json", vision.identity(),
        {utils::path_from_utf8(vision.file)});
}

bool vision_enabled(const strata::ModelCheckpoint& checkpoint, const RecipeOptions& options) {
    const std::string mode = options.get_option("strata_vision");
    if (mode != "cpu" && mode != "gpu" && mode != "off") {
        throw std::invalid_argument("strata_vision must be cpu, gpu or off");
    }
    return mode != "off" && checkpoint.vision.has_value();
}

void prepare_model(const ModelInfo& info, const std::string& backend, const DownloadProgressCallback& progress) {
    std::lock_guard<std::mutex> lock(model_install_mutex);
    const auto checkpoint = strata::model_checkpoint(info.checkpoint(), backend);
    const auto folder = checkpoint.directory();
    const auto& files = checkpoint.files;
    const int total_files = static_cast<int>(files.size());
    std::vector<fs::path> shards;
    for (const auto& file : files) shards.push_back(checkpoint.local_file(folder, file));
    const auto report = [&](DownloadProgress p) {
        if (utils::g_download_cancelled.load() || (progress && !progress(p))) {
            throw std::runtime_error("Strata model download cancelled");
        }
    };
    if (!model_present(checkpoint)) {
        if (!info.registry_source.empty() && info.registry_source != "huggingface") {
            throw std::invalid_argument("Strata currently supports Hugging Face checkpoints only");
        }
        const bool needs_download = !checkpoint.downloads_present(folder);
        if (auto* cfg = RuntimeConfig::global(); cfg && cfg->offline() && needs_download) {
            throw std::runtime_error("Strata model downloads are missing or invalid; pull the model online before using offline mode");
        }
        fs::create_directories(folder);
        if (needs_download) {
            // Same-sized replacement weights must not reuse stale prepared data.
            fs::remove(folder / "pack" / "lemonade-native.json");
            fs::remove(folder / "pack" / "hadamard.txt");
            if (checkpoint.preparation != "ready-pack") fs::remove(folder / "pack" / "experts.bin");
            fs::remove(folder / "lemonade-model.json");
            fs::remove(folder / "metal.json");
            const auto registry = huggingface_repository();
            const auto repository = registry->list(checkpoint.repository, checkpoint.revision);
            for (size_t i = 0; i < files.size(); ++i) {
                auto options = registry_download_options(repository, files[i]);
                prepare_download(shards[i], options);
                fs::create_directories(shards[i].parent_path());
                const auto result = utils::HttpClient::download_file(
                    registry->file_url({checkpoint.repository, {repository.download_revision, false}, files[i]}),
                    utils::path_to_utf8(shards[i]),
                    [&](size_t downloaded, size_t total) {
                        DownloadProgress p;
                        p.file = files[i]; p.file_index = static_cast<int>(i + 1); p.total_files = total_files;
                        p.bytes_downloaded = downloaded; p.bytes_total = total;
                        p.percent = total ? static_cast<int>((i * 100 + downloaded * 100 / total) / total_files) : 0;
                        return !utils::g_download_cancelled.load() && (!progress || progress(p));
                    }, registry->auth_headers(), options);
                if (!result.success) throw std::runtime_error("Strata model download failed: " + result.error_message);
            }
            std::vector<fs::path> downloaded;
            for (const auto& file : files) downloaded.push_back(checkpoint.local_file({}, file));
            strata::record_downloads(folder, "lemonade-model.json", checkpoint.identity(), downloaded);
        }
        if (!strata::pack_present(folder / "pack")) {
            if (checkpoint.preparation == "gguf-pack") {
                strata::prepare_pack(shards, folder / "pack", progress, checkpoint.compat_bf16);
            } else {
                strata::prepare_vocabulary(checkpoint.local_file(folder, checkpoint.native_file), folder / "pack" / "tokenizer.gguf");
                strata::finish_preparation(shards, folder / "pack", checkpoint.preparation);
            }
        }
    }
    DownloadProgress complete;
    complete.file = utils::path_to_utf8(folder / "pack" / "lemonade-native.json");
    complete.percent = 100;
    complete.complete = true;
    report(complete);
}

class StrataOps : public BackendOps {
public:
    void prepare_install(const std::string& staging, const std::string& backend,
                         DownloadProgressCallback progress) const override {
        std::lock_guard<std::mutex> lock(model_install_mutex);
        const auto executable = find_executable_in_dir(staging, strata::spec()->binary);
        if (executable.empty()) throw std::runtime_error("Staged Strata engine executable is missing");
        prepare_engine_data(utils::path_from_utf8(executable).parent_path() / "lemonade-data", backend, progress);
    }

    void populate_metadata(ModelInfo& info, const BackendOpsContext&) const override {
        const auto checkpoint = strata::model_checkpoint(info.checkpoint());
        // The prepared tokenizer rewrites the architecture to llama; read the original GGUF.
        const auto path = checkpoint.local_file(checkpoint.directory(), checkpoint.native_file);
        GgufMetadata metadata;
        if (!read_gguf_metadata(metadata, utils::path_to_utf8(path))) {
            LOG(WARNING, "Strata") << "Could not read model metadata from "
                                   << utils::path_to_utf8(path) << std::endl;
            return;
        }
        info.max_context_window = metadata.context_length;
        info.gguf = std::move(metadata);
    }

    void resolve_runtime_options(const ModelInfo& info, RecipeOptions& options) const override {
        if (!info.checkpoint("draft").empty()) {
            throw std::invalid_argument(
                "Strata does not use llama.cpp draft GGUF checkpoints. Remove checkpoints.draft and use "
                "strata_mtp to select the model's compatible external pack or embedded MTP block.");
        }
        if (!options.get_option("strata_mtp").is_boolean()) {
            throw std::invalid_argument("strata_mtp must be a boolean");
        }
        if (!options.get_option("strata_kv").is_string()) {
            throw std::invalid_argument("strata_kv must be a string");
        }
        if (!options.get_option("strata_experts").is_string()) {
            throw std::invalid_argument("strata_experts must be a string");
        }
        if (!options.get_option("strata_kv_streaming").is_string()) {
            throw std::invalid_argument("strata_kv_streaming must be a string");
        }
        const auto checkpoint = strata::model_checkpoint(info.checkpoint());
        checkpoint.validate_custom_arguments(options.get_option("strata_args").get<std::string>());
        if (!options.has_option("strata_backend")) {
            const auto supported = SystemInfo::get_supported_backends("strata").backends;
            const auto selected = std::find_if(supported.begin(), supported.end(),
                [&](const auto& backend) { return checkpoint.backends.count(backend) != 0; });
            if (selected == supported.end()) throw std::invalid_argument("No compatible Strata backend for " + info.checkpoint());
            options.set_option("strata_backend", *selected);
        }
        checkpoint.validate_backend(options.get_option("strata_backend").get<std::string>());
    }

    std::optional<int64_t> auto_context_size(const ModelInfo& info, const RecipeOptions&) const override {
        // Disk-streamed expert weights do not need to fit entirely in VRAM.
        const auto context = strata::engine_release().default_context;
        return info.max_context_window > 0
            ? std::min<int64_t>(context, info.max_context_window) : context;
    }

    std::string model_support_error(const ModelInfo& info, const json& system) const override {
        const auto checkpoint = strata::model_checkpoint(info.checkpoint());
        const auto variants = system.value("recipes", json::object()).value("strata", json::object())
            .value("backends", json::object());
        for (const auto& backend : checkpoint.backends) {
            if (variants.contains(backend) && variants.at(backend).value("state", "unsupported") != "unsupported") return "";
        }
        return "No supported Strata engine backend for " + info.checkpoint();
    }

    std::string validate_registration_checkpoint(const std::string& checkpoint) const override {
        try {
            strata::model_checkpoint(checkpoint);
            return "";
        } catch (const std::invalid_argument& e) {
            return e.what();
        }
    }

    double streaming_memory_pool_gb(const ModelInfo& info, double system_ram_gb,
                                    double device_pool_gb, const json& hardware) const override {
        if (system_ram_gb <= 0.0) return 0.0;
        if (strata::model_checkpoint(info.checkpoint()).preparation == "native-gguf") return system_ram_gb;
        double dedicated_vram_gb = 0.0;
        for (const auto& [type, devices] : hardware.items()) {
            if (type != "amd_gpu" && type != "nvidia_gpu") continue;
            const auto list = devices.is_array() ? devices : json::array({devices});
            for (const auto& device : list) {
                if (device.is_object() && device.value("available", false) &&
                    !device.value("integrated", type != "nvidia_gpu")) {
                    dedicated_vram_gb = std::max(dedicated_vram_gb, device.value("vram_gb", 0.0));
                }
            }
        }
        return std::max(device_pool_gb, system_ram_gb + dedicated_vram_gb);
    }

    std::string resolve_checkpoint_path(const ModelInfo&, const CheckpointResolveContext& ctx) const override {
        return utils::path_to_utf8(strata::model_checkpoint(ctx.checkpoint).directory() / "pack" / "lemonade-native.json");
    }
    bool is_downloaded(const ModelInfo& info, const BackendOpsContext&) const override {
        if (info.recipe_options.has_option("strata_backend")) {
            return model_present(strata::model_checkpoint(info.checkpoint(),
                info.recipe_options.get_option("strata_backend").get<std::string>()));
        }
        return model_present(strata::model_checkpoint(info.checkpoint()));
    }
    void download_model(const ModelInfo& info, bool, DownloadProgressCallback progress,
                        const BackendOpsContext&) const override {
        auto options = info.recipe_options;
        resolve_runtime_options(info, options);
        const auto report = [&](DownloadProgress p) {
            p.complete = false;
            return !progress || progress(p);
        };
        const std::string backend = options.get_option("strata_backend");
        const auto checkpoint = strata::model_checkpoint(info.checkpoint(), backend);
        const bool enable_vision = vision_enabled(checkpoint, options);
        prepare_model(info, backend, report);
        if (options.get_option("strata_mtp").get<bool>() && checkpoint.mtp && checkpoint.mtp->backends.count(backend)) {
            prepare_draft(*checkpoint.mtp, report);
        }
        if (enable_vision) prepare_vision(*checkpoint.vision, report);
        if (progress) {
            DownloadProgress complete;
            complete.file = "Strata model";
            complete.percent = 100;
            complete.complete = true;
            if (!progress(complete)) throw std::runtime_error("Strata model download cancelled");
        }
    }
    void remove_self_managed_model(const ModelInfo& info) const override {
        std::lock_guard<std::mutex> lock(model_install_mutex);
        fs::remove_all(strata::model_checkpoint(info.checkpoint()).directory());
    }
    bool invalidates_cache_after_download() const override { return true; }
};

}

InstallParams StrataServer::get_install_params(const std::string& backend, const std::string&) {
    const auto release = strata::engine_release();
    return {release.repository, release.asset(get_current_os(), host_arch(), backend,
        backend == "rocm" ? SystemInfo::get_rocm_arch() : "")};
}

void StrataServer::load(const std::string& model_name, const ModelInfo& info,
                       const RecipeOptions& options, bool do_not_upgrade) {
    (void)do_not_upgrade;
    const std::string custom = options.get_option("strata_args");
    const std::string backend = options.get_option("strata_backend");
    const auto checkpoint = strata::model_checkpoint(info.checkpoint(), backend);
    checkpoint.validate_custom_arguments(custom);
    checkpoint.validate_backend(backend);
    const bool enable_vision = vision_enabled(checkpoint, options);
    const bool gpu_vision = enable_vision && options.get_option("strata_vision") == "gpu";
    if (gpu_vision && backend != "rocm" && backend != "metal") {
        throw std::invalid_argument("This Strata package has no GPU picture reader; use strata_vision=cpu");
    }
    backend_manager_->install_backend("strata", backend);
    const bool enable_mtp = options.get_option("strata_mtp");
    const auto folder = checkpoint.directory();
    const auto release = strata::engine_release();
    const int requested = options.get_option("ctx_size");
    const auto executable = BackendUtils::get_backend_binary_path(*strata::spec(), backend);
    const auto engine_dir = utils::path_from_utf8(executable).parent_path();
    auto reader = engine_dir / (gpu_vision && backend == "rocm" ? "strata-vision-hip" : "strata-vision");
#ifdef _WIN32
    reader += ".exe";
#endif
    if (enable_vision && !fs::is_regular_file(reader)) {
        throw std::runtime_error("Strata package has no requested native picture reader: " + utils::path_to_utf8(reader) +
            "; install a complete engine package or use strata_vision=off");
    }
    ServerCommand command;
    command.program = executable;
    ensure_engine_data(engine_dir, backend, nullptr);
    command.args = checkpoint.launch_arguments(release, backend, folder, engine_dir / "lemonade-data", requested, enable_mtp,
        options.get_option("strata_kv").get<std::string>());
    const auto expert_setting = options.get_option("strata_experts").get<std::string>();
    const auto kv_setting = options.get_option("strata_kv").get<std::string>();
    const auto streaming_setting = options.get_option("strata_kv_streaming").get<std::string>();
    const auto extra = utils::parse_custom_args(custom);
    if (streaming_setting != "auto" && std::any_of(extra.begin(), extra.end(), [](const auto& arg) {
            return arg == "--kv-resident" || arg.rfind("--kv-resident=", 0) == 0;
        })) {
        throw std::invalid_argument("Use either strata_kv_streaming or custom --kv-resident, not both");
    }
    auto capacity = backend != "metal"
        ? memory_capacity(backend) : strata::MemoryCapacity{};
    auto plan = checkpoint.memory_plan(release, backend, requested, kv_setting, expert_setting,
        streaming_setting, capacity, !custom.empty());
    prepare_model(info, backend, nullptr);
    if (backend != "metal" && custom.empty() && checkpoint.preparation != "native-gguf" &&
        checkpoint.budget_experts) {
        uint64_t expert_bytes;
        if (checkpoint.preparation == "ready-pack") {
            expert_bytes = fs::file_size(folder / "pack" / "experts.bin");
        } else {
            std::vector<fs::path> shards;
            for (const auto& file : checkpoint.files) shards.push_back(checkpoint.local_file(folder, file));
            expert_bytes = strata::model_expert_bytes(shards);
        }
        capacity.expert_size_gib = static_cast<double>(expert_bytes) / (1024.0 * 1024.0 * 1024.0);
        plan = checkpoint.memory_plan(release, backend, requested, kv_setting, expert_setting,
            streaming_setting, capacity, false);
    }
    if (enable_mtp && checkpoint.mtp && checkpoint.mtp->backends.count(backend)) {
        prepare_draft(*checkpoint.mtp, nullptr);
    }
    if (enable_vision) prepare_vision(*checkpoint.vision, nullptr);
    if (backend == "metal" && checkpoint.preparation == "gguf-pack") {
        std::lock_guard<std::mutex> lock(model_install_mutex);
        std::vector<fs::path> shards;
        for (const auto& file : checkpoint.files) shards.push_back(checkpoint.local_file(folder, file));
        const auto [native, ple] = strata::prepare_metal_native(shards, folder / "metal.gguf");
        const auto flag = std::find(command.args.begin(), command.args.end(), "--native");
        *std::next(flag) = utils::path_to_utf8(native);
        command.args.insert(command.args.end(), {"--ple-gguf", utils::path_to_utf8(ple)});
    }
    command.args.insert(command.args.end(), extra.begin(), extra.end());
    if (backend != "metal" && checkpoint.preparation != "native-gguf") {
        if (gpu_vision) plan.vram_reserve_mib = std::max(plan.vram_reserve_mib, release.vision_gpu_reserve_mib);
        LOG(INFO, "Strata") << "Memory plan: experts=" << plan.experts << " (requested=" << expert_setting
                           << "), resident budget=" << plan.resident_budget_gib << " GiB, PLE policy="
                           << (std::any_of(extra.begin(), extra.end(), [](const auto& arg) {
                                  return arg == "--ple-io" || arg.rfind("--ple-io=", 0) == 0;
                              }) ? "custom" : plan.ple_direct ? "direct" : "engine")
                           << ", pool workers=" << plan.pool_workers
                           << ", KV resident window=" << plan.kv_resident << " (requested=" << streaming_setting
                           << "), total RAM=" << capacity.total_ram_gib << " GiB, total VRAM="
                           << capacity.total_vram_gib << " GiB, VRAM reserve="
                           << plan.vram_reserve_mib << " MiB" << std::endl;
        if (plan.experts != "mmap" || plan.kv_resident > 0) {
            LOG(WARNING, "Strata") << "Native expert/KV allocations are not limited by currently available RAM or commit. "
                "If memory is tight, use --strata-experts mmap and/or --strata-kv-streaming off." << std::endl;
        }
        plan.apply(command.args);
    }
    const auto build = utils::JsonUtils::load_from_file(utils::path_to_utf8(engine_dir / "BUILD.json"));
    std::string library_path = utils::path_to_utf8(engine_dir);
    for (const auto& entry : build.value("lib_dirs", json::array())) {
        const auto relative = utils::path_from_utf8(entry.get<std::string>());
        if (relative.is_absolute() || std::find(relative.begin(), relative.end(), fs::path("..")) != relative.end()) {
            throw std::runtime_error("Strata library path escapes its engine package");
        }
#ifdef _WIN32
        library_path += ";" + utils::path_to_utf8(engine_dir / relative);
#else
        library_path += ":" + utils::path_to_utf8(engine_dir / relative);
#endif
    }
#ifdef _WIN32
    const std::string variable = "PATH";
    const std::string separator = ";";
#elif defined(__APPLE__)
    const std::string variable = "DYLD_LIBRARY_PATH";
    const std::string separator = ":";
#else
    const std::string variable = "LD_LIBRARY_PATH";
    const std::string separator = ":";
#endif
    const auto inherited = utils::get_environment_variable_utf8(variable);
    command.env = {{variable, library_path + (inherited.empty() ? "" : separator + inherited)}};
    std::optional<strata::VisionCommand> vision;
    if (enable_vision) {
        command.args.push_back("--vision");
        const int limit = gpu_vision ? release.vision_gpu_max_tokens : release.vision_cpu_max_tokens;
        ServerCommand encoder;
        encoder.program = utils::path_to_utf8(reader);
        encoder.args = {"--mmproj", utils::path_to_utf8(checkpoint.vision->path()), "--model",
            utils::path_to_utf8(checkpoint.local_file(folder, checkpoint.native_file)),
            "--max-tokens", std::to_string(limit)};
        if (gpu_vision) encoder.args.push_back("--gpu");
        encoder.env = command.env;
        vision = strata::VisionCommand{encoder, limit};
        if (gpu_vision && checkpoint.preparation != "native-gguf" &&
            std::none_of(command.args.begin(), command.args.end(), [](const auto& arg) {
                return arg == "--vram-reserve-mib" || arg.rfind("--vram-reserve-mib=", 0) == 0;
            })) {
            command.args.insert(command.args.end(), {"--vram-reserve-mib", std::to_string(release.vision_gpu_reserve_mib)});
        }
    }
    start_server(std::make_unique<strata::StrataProcess>(folder / "pack" / "tokenizer.gguf", model_name,
        std::move(vision), [this] { return load_cancelled(); }), command);
}

json StrataServer::native_request(const json& request, bool responses) {
    const auto process = std::dynamic_pointer_cast<strata::StrataProcess>(acquire_process());
    if (!is_backend_alive() || !process) {
        if (process && !was_watchdog_triggered()) {
            request_backend_reset_from_watchdog("Strata native engine exited before request");
        }
        if (was_watchdog_triggered()) return create_watchdog_reset_response();
        return ErrorResponse::from_exception(ModelNotLoadedException(server_name_));
    }
    BackendRequestScope scope(*this, BackendRequestKind::NonStreaming);
    const auto cancel = current_request_cancel_context();
    try {
        if (cancel.cancelled()) throw std::runtime_error("Request cancelled by client");
        auto result = process->request(request, responses, [&] { return cancel.cancelled(); });
        note_backend_activity();
        if (cancel.cancelled()) throw std::runtime_error("Request cancelled by client");
        return result;
    } catch (const std::invalid_argument& e) {
        LOG(WARNING, "Strata") << "Invalid native request: " << e.what() << std::endl;
        return ErrorResponse::create(e.what(), ErrorType::INVALID_REQUEST);
    } catch (const std::exception& e) {
        if (cancel.cancelled()) {
            LOG(WARNING, "Strata") << "Native request cancelled by client" << std::endl;
            return ErrorResponse::create("Request cancelled by client", ErrorType::INVALID_REQUEST);
        }
        LOG(ERROR, "Strata") << "Native inference failed: " << e.what() << std::endl;
        return ErrorResponse::create(e.what(), ErrorType::BACKEND_ERROR);
    }
}

json StrataServer::chat_completion(const json& request) { return native_request(request, false); }
json StrataServer::responses(const json& request) { return native_request(request, true); }

void StrataServer::forward_streaming_request(const std::string& endpoint, const std::string& request_body,
                                             httplib::DataSink& sink, bool sse, long timeout_seconds,
                                             TelemetryCallback telemetry_callback) {
    const std::string error_event = endpoint == "/v1/responses" ? "event: error\n" : "";
    const auto process = std::dynamic_pointer_cast<strata::StrataProcess>(acquire_process());
    if (!is_backend_alive() || !process) {
        if (was_watchdog_triggered() || process) {
            if (!was_watchdog_triggered()) {
                request_backend_reset_from_watchdog("Strata native engine exited before streaming request");
            }
            throw BackendStreamRetryableReset(get_watchdog_reset_reason());
        }
        const auto error = error_event + "data: " + ErrorResponse::from_exception(ModelNotLoadedException(server_name_)).dump() + "\n\n";
        sink.write(error.data(), error.size());
        if (sink.done) sink.done();
        return;
    }
    BackendRequestScope scope(*this, BackendRequestKind::Streaming);
    const auto cancel = current_request_cancel_context();
    const auto started = std::chrono::steady_clock::now();
    bool write_failed = false;
    const auto timed_out = [&] {
        return timeout_seconds > 0 &&
            std::chrono::steady_clock::now() - started >= std::chrono::seconds(timeout_seconds);
    };
    const auto cancelled = [&] {
        return write_failed || cancel.cancelled() || (sink.is_writable && !sink.is_writable()) || timed_out();
    };
    bool completed = false;
    const auto complete = [&](const json& result) {
        if (completed) return;
        completed = true;
        note_backend_activity();
        if (telemetry_callback) {
            auto telemetry = StreamingProxy::extract_telemetry(result);
            if (result.contains("error")) telemetry.error_message = result["error"].value("message", "Strata inference failed");
            else if (cancelled()) telemetry.error_message = timed_out() ? "Strata stream timed out" : "Client disconnected during stream";
            telemetry_callback(telemetry);
        }
    };
    httplib::DataSink native_sink;
    native_sink.write = [&](const char* data, size_t size) {
        if (cancelled()) return false;
        const bool written = sink.write(data, size);
        if (written) {
            set_streaming(true);
            note_backend_activity();
        } else write_failed = true;
        return written;
    };
    native_sink.is_writable = [&] { return !cancelled(); };
    native_sink.done = [&] { if (sink.done) sink.done(); };
    try {
        if (!sse || (endpoint != "/v1/chat/completions" && endpoint != "/v1/responses")) {
            throw std::invalid_argument("Unsupported Strata streaming endpoint or transport");
        }
        process->stream(json::parse(request_body), endpoint == "/v1/responses", native_sink, cancelled, complete);
    } catch (const std::exception& e) {
        LOG(ERROR, "Strata") << "Native streaming request failed: " << e.what() << std::endl;
        const auto type = dynamic_cast<const std::invalid_argument*>(&e) || dynamic_cast<const json::exception*>(&e)
            ? ErrorType::INVALID_REQUEST : ErrorType::BACKEND_ERROR;
        const auto error = ErrorResponse::create(e.what(), type);
        const auto data = error_event + "data: " + error.dump() + "\n\n";
        sink.write(data.data(), data.size());
        complete(error);
        if (sink.done) sink.done();
    }
}

void StrataServer::unload() { stop_server(); }
StrataServer::~StrataServer() { stop_server(); }

namespace strata {

std::unique_ptr<WrappedServer> create(const BackendContext& ctx) { return make_server<StrataServer>(ctx); }
const BackendSpec* spec() {
    static const BackendSpec value("strata", descriptor.binary, StrataServer::get_install_params, false, true);
    return &value;
}
const BackendOps* ops() { return single_ops<StrataOps>(); }

}
}

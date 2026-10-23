#include "lemon/backends/strata/strata_runtime.h"

#include "lemon/backends/backend_utils.h"
#include "lemon/models/registry_types.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/url_utils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;

namespace lemon::backends::strata {
namespace {

using json = nlohmann::json;

json metadata() {
    return utils::JsonUtils::load_from_file(utils::get_resource_path("resources/strata.json"));
}

void validate_component(const std::string& value) {
    if (value.empty() || value == "." || value == ".." ||
        value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") != std::string::npos) {
        throw std::invalid_argument("Invalid Strata metadata path component: " + value);
    }
}

void validate_repository(const std::string& repository) {
    const auto slash = repository.find('/');
    if (slash == std::string::npos) throw std::invalid_argument("Strata requires an owner/model repository");
    validate_component(repository.substr(0, slash));
    validate_component(repository.substr(slash + 1));
}

void validate_relative_file(const std::string& file) {
    const auto path = utils::path_from_utf8(file);
    if (file.empty() || path.is_absolute() || path.has_root_name() || file.find('\\') != std::string::npos) {
        throw std::invalid_argument("Strata data files must be repository-relative");
    }
    for (const auto& component : path) validate_component(utils::path_to_utf8(component));
}

bool downloads_match(const fs::path& directory, const std::string& name, const json& identity,
                     const std::vector<fs::path>& files,
                     std::initializer_list<const char*> ignored_fields = {},
                     bool weights_only = false) {
    const auto receipt = directory / name;
    if (!fs::is_regular_file(receipt)) return false;
    const auto record = utils::JsonUtils::load_from_file(utils::path_to_utf8(receipt));
    if (!record.is_object() || !record.value("identity", json{}).is_object() ||
        !record.contains("sizes") || !record.at("sizes").is_object()) return false;
    auto recorded_identity = record.at("identity");
    auto expected_identity = identity;
    for (const auto* field : ignored_fields) {
        recorded_identity.erase(field);
        expected_identity.erase(field);
    }
    if (weights_only) {
        for (auto* value : {&recorded_identity, &expected_identity}) {
            auto& entries = value->at("files");
            for (auto it = entries.begin(); it != entries.end();) {
                if (!it.value().value("source", "").empty()) it = entries.erase(it);
                else ++it;
            }
        }
    }
    if (recorded_identity != expected_identity) return false;
    return std::all_of(files.begin(), files.end(), [&](const fs::path& file) {
        const auto key = file.generic_string();
        return record.at("sizes").contains(key) && record.at("sizes").at(key).is_number_unsigned() &&
            record.at("sizes").at(key).get<uint64_t>() > 0 && fs::is_regular_file(directory / file) &&
            fs::file_size(directory / file) == record.at("sizes").at(key).get<uint64_t>();
    });
}

fs::path shared_cache_directory(const fs::path& root, const std::string& receipt, const json& identity) {
    const auto shared = root / "shared";
    if (fs::exists(shared) || !fs::is_directory(root)) return shared;
    std::vector<fs::path> candidates;
    for (const auto& entry : fs::directory_iterator(root)) {
        if (entry.is_directory()) candidates.push_back(entry.path());
    }
    std::sort(candidates.begin(), candidates.end());
    for (const auto& candidate : candidates) {
        if (downloads_match(candidate, receipt, identity, {}, {"release"}, receipt == "lemonade-draft.json")) {
            return candidate;
        }
    }
    return shared;
}

void replace(std::string& value, const std::string& key, const std::string& replacement) {
    size_t at = 0;
    while ((at = value.find(key, at)) != std::string::npos) {
        value.replace(at, key.size(), replacement);
        at += replacement.size();
    }
}

}

std::set<std::string> engine_data_files() {
    std::set<std::string> files;
    const auto data = metadata();
    for (const auto& [repository, model] : data.at("models").items()) {
        if (model.contains("expert_profile")) files.insert(model.at("expert_profile").get<std::string>());
        if (model.contains("mtp")) {
            for (const auto& [name, file] : model.at("mtp").at("files").items()) {
                const auto source = file.value("source", "");
                if (!source.empty()) files.insert(source);
            }
        }
    }
    for (const auto& file : files) validate_relative_file(file);
    return files;
}

namespace {
json engine_data_identity(const std::string& backend) {
    return {{"repository", engine_release().repository},
        {"release", BackendUtils::get_backend_version("strata", backend)}, {"files", engine_data_files()}};
}
}

bool engine_data_present(const fs::path& directory, const std::string& backend) {
    const auto files = engine_data_files();
    return downloads_match(directory, "lemonade-data.json", engine_data_identity(backend),
        std::vector<fs::path>(files.begin(), files.end()));
}

void prepare_engine_data(const fs::path& directory, const std::string& backend,
    const std::function<void(const std::string&, const fs::path&)>& download) {
    const auto files = engine_data_files();
    fs::create_directories(directory);
    fs::remove(directory / "lemonade-data.json");
    for (const auto& file : files) {
        const auto path = directory / utils::path_from_utf8(file);
        fs::create_directories(path.parent_path());
        fs::remove(path);
        download(engine_release().data_url(BackendUtils::get_backend_version("strata", backend), file), path);
    }
    record_downloads(directory, "lemonade-data.json", engine_data_identity(backend),
        std::vector<fs::path>(files.begin(), files.end()));
}

void record_downloads(const fs::path& directory, const std::string& receipt, const json& identity,
                      const std::vector<fs::path>& files) {
    validate_component(receipt);
    json sizes = json::object();
    for (const auto& file : files) {
        validate_relative_file(file.generic_string());
        const auto path = directory / file;
        if (!fs::is_regular_file(path) || fs::file_size(path) == 0) {
            throw std::runtime_error("Missing or empty Strata download: " + utils::path_to_utf8(path));
        }
        sizes[file.generic_string()] = fs::file_size(path);
    }
    const auto temporary = directory / (receipt + ".tmp");
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << json({{"identity", identity}, {"sizes", sizes}}).dump(2);
    output.close();
    if (!output) throw std::runtime_error("Could not write Strata download receipt");
    std::error_code error;
    fs::remove(directory / receipt, error);
    if (error) throw std::runtime_error("Could not replace Strata download receipt: " + error.message());
    fs::rename(temporary, directory / receipt);
}

namespace {

int positive_integer(const json& value, int maximum = std::numeric_limits<int>::max()) {
    if (!value.is_number_integer() || value <= 0 || value > maximum) {
        throw std::invalid_argument("Strata metadata requires a positive bounded integer");
    }
    return value.get<int>();
}

double positive_number(const json& value) {
    if (!value.is_number() || !std::isfinite(value.get<double>()) || value.get<double>() <= 0) {
        throw std::invalid_argument("Strata memory recommendations must be finite positive numbers");
    }
    return value.get<double>();
}

}

const std::set<std::string>& managed_arguments() {
    static const std::set<std::string> flags = {
        "--serve", "--tokens", "--pack", "--native", "--native-dense-gguf", "--ple-gguf", "--embd-gguf",
        "--expert-profile", "--mtp", "--mtp-draft-vocab", "--kv", "--mmap-experts", "--resident-experts", "--resident-cpu-experts",
        "--max-context", "--ctx", "--model", "--backend", "--batch", "--slots", "--batch-groups",
        "--host", "--port", "--vision", "--emb", "--n", "--max-new", "--force", "--dump-full-logits",
        "--bench-pp", "--bench-tg",
    };
    return flags;
}

EngineRelease parse_engine_release(const json& data) {
    if (data.at("protocol") != 1) throw std::invalid_argument("Unsupported Strata pipe protocol; update the C++ adapter");
    EngineRelease result;
    result.repository = data.at("repository").get<std::string>();
    result.asset_pattern = data.at("asset_pattern").get<std::string>();
    result.targets = data.at("targets").get<decltype(result.targets)>();
    result.arguments = data.at("arguments").get<std::vector<std::string>>();
    result.default_context = positive_integer(data.at("default_context"));
    const auto& tuning = data.at("memory_tuning");
    result.expert_ram_headroom_gib = positive_integer(tuning.at("expert_ram_headroom_gib"));
    result.expert_resident_headroom_gib = positive_integer(tuning.at("expert_resident_headroom_gib"));
    result.expert_min_budget_gib = positive_integer(tuning.at("expert_min_budget_gib"));
    result.pool_workers_cap = positive_integer(tuning.at("pool_workers_cap"));
    if (result.default_context <= 8) throw std::invalid_argument("Strata default context must exceed eight tokens");
    const auto& kv = data.at("kv_cache");
    result.kv_short_context = positive_integer(kv.at("short_context"));
    result.kv_short_format = kv.at("short_format").get<std::string>();
    result.kv_long_format = kv.at("long_format").get<std::string>();
    result.kv_streaming_context = positive_integer(kv.at("streaming_context"));
    result.kv_resident_context = positive_integer(kv.at("resident_context"));
    result.kv_streaming_headroom_gb = positive_number(kv.at("streaming_headroom_gb"));
    const auto& reserve = data.at("vram_reserve");
    result.vram_small_gpu_gib = positive_number(reserve.at("small_gpu_gib"));
    result.vram_reserve_fraction = positive_number(reserve.at("fraction"));
    if (result.vram_reserve_fraction > 1) {
        throw std::invalid_argument("Strata VRAM reserve fraction must not exceed one");
    }
    result.vram_small_reserve_mib = positive_integer(reserve.at("small_reserve_mib"));
    result.vram_large_reserve_mib = positive_integer(reserve.at("large_reserve_mib"));
    const auto& vision = data.at("vision");
    result.vision_cpu_max_tokens = positive_integer(vision.at("cpu_max_tokens"), std::numeric_limits<int>::max() / 4);
    result.vision_gpu_max_tokens = positive_integer(vision.at("gpu_max_tokens"), std::numeric_limits<int>::max() / 4);
    result.vision_gpu_reserve_mib = positive_integer(vision.at("gpu_reserve_mib"));
    if (result.kv_resident_context >= result.kv_streaming_context) {
        throw std::invalid_argument("Strata KV resident window must be smaller than the streaming threshold");
    }
    result.kv_formats = kv.at("formats").get<decltype(result.kv_formats)>();
    result.kv_layers = positive_integer(kv.at("layers"));
    for (const auto& [format, bytes] : kv.at("cell_bytes").items()) {
        result.kv_cell_bytes.emplace(format, positive_integer(bytes));
    }
    if (result.kv_formats.empty()) throw std::invalid_argument("Strata KV formats must name supported backends");
    for (const auto& [backend, formats] : result.kv_formats) {
        if (!formats.count(result.kv_short_format) || !formats.count(result.kv_long_format)) {
            throw std::invalid_argument("Strata KV recommendations must be supported formats");
        }
        for (const auto& format : formats) {
            if (!result.kv_cell_bytes.count(format)) {
                throw std::invalid_argument("Strata KV memory recommendations require every supported cell size");
            }
        }
    }
    validate_repository(result.repository);
    for (const auto& argument : result.arguments) {
        if (managed_arguments().count(argument.substr(0, argument.find('=')))) {
            throw std::invalid_argument("Strata release tuning arguments cannot override " + argument);
        }
    }
    if (result.repository.empty() || result.targets.empty() ||
        result.asset_pattern.empty()) {
        throw std::invalid_argument("Incomplete Strata release metadata");
    }
    return result;
}

EngineRelease engine_release() {
    return parse_engine_release(metadata().at("engine"));
}

std::string EngineRelease::data_url(const std::string& tag, const std::string& file) const {
    validate_repository(repository);
    validate_relative_file(file);
    if (tag.empty() || tag == "latest") throw std::invalid_argument("Strata data requires an exact release tag");
    return "https://raw.githubusercontent.com/" + repository + "/" + utils::url_encode(tag) + "/" + file;
}

std::string EngineRelease::asset(const std::string& os, const std::string& arch,
                                 const std::string& backend, const std::string& gpu_arch) const {
    const auto platform = os + "-" + arch;
    const auto target = targets.find(platform);
    if (target != targets.end()) {
        const auto family = target->second.find(backend);
        if (family != target->second.end()) {
            const auto variant = family->second.find(gpu_arch.empty() ? "default" : gpu_arch);
            if (variant != family->second.end()) {
                auto name = asset_pattern;
                replace(name, "{platform}", platform);
                replace(name, "{variant}", variant->second);
                auto literal = name;
                literal.erase(std::remove(literal.begin(), literal.end(), '*'), literal.end());
                validate_component(literal);
                return name;
            }
        }
    }
    throw std::invalid_argument("No Strata engine for " + platform + "/" + backend + "/" + gpu_arch);
}

DraftPack parse_draft_pack(const json& data) {
    DraftPack result;
    result.repository = data.at("repository").get<std::string>();
    result.revision = models::huggingface_descriptor().default_revision;
    result.backends = data.at("backends").get<std::set<std::string>>();
    result.arguments = data.at("arguments").get<std::vector<std::string>>();
    validate_repository(result.repository);
    if (result.backends.empty()) throw std::invalid_argument("Strata draft packs require supported backends");
    std::set<std::string> basenames;
    for (const auto& [name, file] : data.at("files").items()) {
        validate_relative_file(name);
        const auto path = utils::path_from_utf8(name);
        auto basename = utils::path_to_utf8(path.filename());
        std::transform(basename.begin(), basename.end(), basename.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!basenames.insert(basename).second || basename == "lemonade-draft.json") {
            throw std::invalid_argument("Strata draft filenames collide");
        }
        const auto source = file.value("source", "");
        if (!source.empty()) {
            validate_relative_file(source);
            if (path.filename() != "draft_vocab.bin") {
                throw std::invalid_argument("Only Strata draft vocabulary can use an engine-owned MTP source");
            }
        }
        result.files.emplace(name, DraftFile{source});
    }
    for (const auto& required : {"dense.bin", "dense.txt", "experts.bin", "draft_vocab.bin"}) {
        if (!basenames.count(required)) throw std::invalid_argument("Missing Strata draft file: " + std::string(required));
    }
    for (const auto& argument : result.arguments) {
        if (managed_arguments().count(argument.substr(0, argument.find('=')))) {
            throw std::invalid_argument("Strata draft tuning cannot override " + argument);
        }
    }
    return result;
}

fs::path DraftPack::directory() const {
    const auto root = utils::path_from_utf8(utils::get_hf_cache_dir()) /
        registry_repo_cache_dir_name(repository, models::huggingface_descriptor().id) / "strata-drafts" /
        revision;
    return shared_cache_directory(root, "lemonade-draft.json", identity());
}

json DraftPack::identity() const {
    json entries = json::object();
    for (const auto& [name, file] : files) {
        entries[name] = {{"source", file.source}};
    }
    return {{"repository", repository}, {"revision", revision}, {"files", entries}};
}

bool DraftPack::present() const {
    return std::all_of(files.begin(), files.end(), [&](const auto& entry) {
        return !entry.second.source.empty() || file_present(entry.first);
    });
}

bool DraftPack::file_present(const std::string& name) const {
    if (!files.at(name).source.empty()) return false;
    return downloads_match(directory(), "lemonade-draft.json", identity(),
        {utils::path_from_utf8(name).filename()}, {"release"}, true);
}

VisionPack parse_vision_pack(const json& data) {
    VisionPack result{data.at("repository").get<std::string>(),
        models::huggingface_descriptor().default_revision, data.at("file").get<std::string>()};
    validate_repository(result.repository);
    validate_component(result.revision);
    validate_component(result.file);
    return result;
}

fs::path VisionPack::directory() const {
    const auto root = utils::path_from_utf8(utils::get_hf_cache_dir()) /
        registry_repo_cache_dir_name(repository, models::huggingface_descriptor().id) / "strata-vision" /
        revision;
    return shared_cache_directory(root, "lemonade-vision.json", identity());
}

fs::path VisionPack::path() const { return directory() / utils::path_from_utf8(file); }

json VisionPack::identity() const {
    return {{"repository", repository}, {"revision", revision}, {"file", file}};
}

bool VisionPack::present() const {
    return downloads_match(directory(), "lemonade-vision.json", identity(), {utils::path_from_utf8(file)}, {"release"});
}

ModelCheckpoint parse_model_checkpoint(const std::string& checkpoint, const json& models) {
    const auto colon = checkpoint.rfind(':');
    if (colon == std::string::npos) throw std::invalid_argument("Strata requires a repository:quant checkpoint");
    ModelCheckpoint result;
    result.repository = checkpoint.substr(0, colon);
    result.quant = checkpoint.substr(colon + 1);
    validate_component(result.quant);
    validate_repository(result.repository);
    if (!models.contains(result.repository)) throw std::invalid_argument("Unsupported Strata repository: " + result.repository);
    const auto& model = models.at(result.repository);
    const auto& quants = model.at("quants");
    if (!quants.contains(result.quant)) throw std::invalid_argument("Unsupported Strata quant: " + result.quant);
    result.revision = models::huggingface_descriptor().default_revision;
    result.files = quants.at(result.quant).get<std::vector<std::string>>();
    auto settings = model;
    if (model.contains("quant_options") && model.at("quant_options").contains(result.quant)) {
        settings.update(model.at("quant_options").at(result.quant));
    }
    result.preparation = settings.value("preparation", "gguf-pack");
    result.compat_bf16 = settings.value("compat_bf16", false);
    result.backends = settings.value("backends", std::set<std::string>{"cuda", "cuda13", "rocm", "metal"});
    result.replace_arguments = settings.contains("arguments");
    result.arguments = settings.value("arguments", std::vector<std::string>{});
    result.native_file = settings.value("native_file", result.files.empty() ? "" : result.files.front());
    result.ple_file = settings.value("ple_file", "");
    result.embedding_file = settings.value("embedding_file", "");
    result.full_expert_arena = settings.value("full_expert_arena", false);
    result.budget_experts = settings.value("budget_experts", false);
    if (settings.contains("recommended_ram_gb")) {
        result.recommended_ram_gb = positive_number(settings.at("recommended_ram_gb"));
    }
    const auto& draft_model = settings.contains("mtp_from")
        ? models.at(settings.at("mtp_from").get<std::string>()) : settings;
    if (draft_model.contains("mtp")) result.mtp = parse_draft_pack(draft_model.at("mtp"));
    if (settings.contains("embedded_mtp_drafts")) {
        result.embedded_mtp_drafts = positive_integer(settings.at("embedded_mtp_drafts"), 7);
        if (result.preparation != "native-gguf" || result.mtp) {
            throw std::invalid_argument("Embedded Strata MTP requires a direct GGUF without an external draft pack");
        }
    }
    const auto& vision_model = settings.contains("vision_from")
        ? models.at(settings.at("vision_from").get<std::string>()) : settings;
    if (vision_model.contains("vision")) result.vision = parse_vision_pack(vision_model.at("vision"));
    if (result.preparation != "gguf-pack" && result.preparation != "native-gguf" && result.preparation != "ready-pack") {
        throw std::invalid_argument("Unsupported Strata preparation mode: " + result.preparation);
    }
    const auto& profile_model = model.contains("expert_profile_from")
        ? models.at(model.at("expert_profile_from").get<std::string>()) : model;
    if (profile_model.contains("expert_profile")) {
        result.profile_file = profile_model.at("expert_profile").get<std::string>();
        validate_relative_file(result.profile_file);
    }
    if (result.revision.empty() || result.files.empty() || result.backends.empty() || result.native_file.empty()) {
        throw std::invalid_argument("Incomplete Strata checkpoint metadata");
    }
    for (const auto& argument : result.arguments) {
        const auto flag = argument.substr(0, argument.find('='));
        if (managed_arguments().count(flag)) {
            throw std::invalid_argument("Strata model tuning arguments cannot override " + flag);
        }
    }
    if (result.preparation == "native-gguf" && result.files.size() != 1) {
        throw std::invalid_argument("Direct Strata models require exactly one GGUF");
    }
    for (const auto& input : {result.native_file, result.ple_file, result.embedding_file}) {
        if (!input.empty() && std::find(result.files.begin(), result.files.end(), input) == result.files.end()) {
            throw std::invalid_argument("Strata input is not a checkpoint download: " + input);
        }
    }
    if (result.preparation == "ready-pack") {
        for (const auto& required : {"pack/dense.bin", "pack/index.txt", "pack/native_experts.txt", "pack/experts.bin"}) {
            if (std::find(result.files.begin(), result.files.end(), required) == result.files.end()) {
                throw std::invalid_argument("Missing ready-pack download: " + std::string(required));
            }
        }
        if (result.ple_file.empty() || result.embedding_file.empty()) {
            throw std::invalid_argument("Strata ready packs require PLE and embedding inputs");
        }
    }
    std::set<std::string> basenames;
    for (const auto& file : result.files) {
        validate_relative_file(file);
        const auto path = utils::path_from_utf8(file);
        auto basename = result.preparation == "ready-pack" ? file : utils::path_to_utf8(path.filename());
        std::transform(basename.begin(), basename.end(), basename.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (basename == "expert-profile.bin" || basename == "lemonade-model.json" ||
            basename == "lemonade-model.json.tmp" || basename == "pack" || basename == "metal.gguf" ||
            basename == "metal.gguf.part" || basename == "metal.json" ||
            basename == "pack/lemonade-native.json" || basename == "pack/tokenizer.gguf" ||
            !basenames.insert(basename).second) {
            throw std::invalid_argument("Conflicting Strata shard filename: " + file);
        }
    }
    return result;
}

ModelCheckpoint model_checkpoint(const std::string& checkpoint, const std::string& backend) {
    const auto data = metadata();
    auto result = parse_model_checkpoint(checkpoint, data.at("models"));
    const auto selected = backend.empty() ? *result.backends.begin() : backend;
    result.validate_backend(selected);
    return result;
}

fs::path ModelCheckpoint::directory() const {
    return utils::path_from_utf8(utils::get_hf_cache_dir()) /
        registry_repo_cache_dir_name(repository, models::huggingface_descriptor().id) / "strata" / quant;
}

json ModelCheckpoint::identity() const {
    return {{"repository", repository}, {"revision", revision}, {"quant", quant}, {"files", files},
        {"preparation", preparation}, {"compat_bf16", compat_bf16},
        {"native_file", native_file}, {"ple_file", ple_file}, {"embedding_file", embedding_file}};
}

bool ModelCheckpoint::downloads_present(const fs::path& folder) const {
    std::vector<fs::path> paths;
    for (const auto& file : files) paths.push_back(local_file({}, file));
    return downloads_match(folder, "lemonade-model.json", identity(), paths, {"release", "expert_profile"});
}

fs::path ModelCheckpoint::local_file(const fs::path& folder, const std::string& file) const {
    const auto path = utils::path_from_utf8(file);
    return folder / (preparation == "ready-pack" ? path : path.filename());
}

void ModelCheckpoint::validate_backend(const std::string& backend) const {
    if (!backends.count(backend)) {
        throw std::invalid_argument(repository + ":" + quant + " does not support Strata backend " + backend);
    }
}

void ModelCheckpoint::validate_custom_arguments(const std::string& arguments) const {
    const auto error = utils::validate_custom_args(arguments, managed_arguments());
    if (!error.empty()) throw std::invalid_argument("Invalid Strata arguments:\n" + error);
    if (preparation != "native-gguf") return;
    // The direct-GGUF engine otherwise logs and ignores unknown serve-mode flags.
    static const std::map<std::string, bool> options = {
        {"--chunk", true}, {"--cpu-threads", true}, {"--draft-vocab", true},
        {"--expert-slots", true}, {"--expert-vram", true}, {"--gpu", true},
        {"--hybrid-rows", true}, {"--no-graph", false}, {"--no-token-graph", false},
        {"--no-warmup", false}, {"--pool-workers", true}, {"--prefill", true},
    };
    const auto tokens = utils::parse_custom_args(arguments);
    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto option = options.find(tokens[i]);
        if (option == options.end()) {
            throw std::invalid_argument("Unsupported Strata direct-GGUF argument: " + tokens[i] +
                "; use a supported engine flag with space-separated values");
        }
        if (option->second && (++i == tokens.size() || tokens[i].rfind("--", 0) == 0)) {
            throw std::invalid_argument("Strata argument requires a value: " + option->first);
        }
    }
}

std::vector<std::string> ModelCheckpoint::launch_arguments(const EngineRelease& release, const std::string& backend,
                                                          const fs::path& folder, const fs::path& engine_data,
                                                          int context, bool enable_mtp,
                                                          const std::string& kv_cache) const {
    validate_backend(backend);
    const int resolved_context = context > 0 ? context : release.default_context;
    const auto native = local_file(folder, native_file);
    std::vector<std::string> args = {"--serve", "--native", utils::path_to_utf8(native),
        "--max-context", std::to_string(resolved_context)};
    const auto formats = release.kv_formats.find(backend);
    if (preparation != "native-gguf" && formats != release.kv_formats.end()) {
        const auto format = kv_cache == "auto"
            ? (resolved_context <= release.kv_short_context ? release.kv_short_format : release.kv_long_format)
            : kv_cache;
        if (!formats->second.count(format)) throw std::invalid_argument("Unsupported Strata KV cache format: " + format);
        args.insert(args.end(), {"--kv", format});
    } else if (kv_cache != "auto") {
        throw std::invalid_argument("Explicit Strata KV cache selection is not supported for this model/backend");
    }
    if (preparation != "native-gguf") {
        args.insert(args.end(), {"--mmap-experts", "--pack", utils::path_to_utf8(folder / "pack")});
        if (!profile_file.empty()) {
            args.insert(args.end(), {"--expert-profile", utils::path_to_utf8(engine_data / profile_file)});
        }
    }
    if (preparation == "ready-pack") {
        args.insert(args.end(), {"--native-dense-gguf", utils::path_to_utf8(native)});
    }
    if (!ple_file.empty()) args.insert(args.end(), {"--ple-gguf", utils::path_to_utf8(local_file(folder, ple_file))});
    if (!embedding_file.empty()) args.insert(args.end(), {"--embd-gguf", utils::path_to_utf8(local_file(folder, embedding_file))});
    const auto& tuning = replace_arguments ? arguments : release.arguments;
    args.insert(args.end(), tuning.begin(), tuning.end());
    if (embedded_mtp_drafts > 0) {
        args.insert(args.end(), {"--mtp", std::to_string(enable_mtp ? embedded_mtp_drafts : 0)});
    }
    if (enable_mtp && mtp && mtp->backends.count(backend)) {
        args.insert(args.end(), {"--mtp", utils::path_to_utf8(mtp->directory())});
        for (const auto& [name, file] : mtp->files) {
            if (!file.source.empty()) {
                args.insert(args.end(), {"--mtp-draft-vocab", utils::path_to_utf8(engine_data / file.source)});
            }
        }
        args.insert(args.end(), mtp->arguments.begin(), mtp->arguments.end());
    }
    return args;
}

void MemoryPlan::apply(std::vector<std::string>& arguments) const {
    const auto has_option = [&](const std::string& flag) {
        return std::any_of(arguments.begin(), arguments.end(), [&](const std::string& argument) {
            return argument == flag || argument.rfind(flag + "=", 0) == 0;
        });
    };
    if (!experts.empty()) {
        const auto flag = std::find(arguments.begin(), arguments.end(), "--mmap-experts");
        if (flag == arguments.end()) throw std::logic_error("Strata launch is missing its expert memory mode");
        if (experts == "arena") arguments.erase(flag);
        else if (experts == "resident") *flag = "--resident-experts";
        else if (experts == "budget") arguments.erase(flag);
        else if (experts != "mmap") throw std::logic_error("Invalid Strata expert memory plan");
    }
    if (ple_direct && !has_option("--ple-io")) {
        arguments.insert(arguments.end(), {"--ple-io", "direct"});
    }
    if (resident_budget_gib > 0 && !has_option("--resident-budget-gib")) {
        arguments.insert(arguments.end(), {"--resident-budget-gib", std::to_string(resident_budget_gib)});
    }
    if (pool_workers > 0 && !has_option("--pool-workers") && !has_option("--cpu-threads")) {
        arguments.insert(arguments.end(), {"--pool-workers", std::to_string(pool_workers)});
    }
    if (kv_resident > 0) {
        const auto raw_kv = std::find_if(arguments.begin(), arguments.end(), [](const auto& arg) {
            return arg == "--kv-resident" || arg.rfind("--kv-resident=", 0) == 0;
        });
        if (raw_kv != arguments.end()) {
            throw std::invalid_argument("Use either strata_kv_streaming or custom --kv-resident, not both");
        }
        arguments.insert(arguments.end(), {"--kv-resident", std::to_string(kv_resident)});
    }
    if (vram_reserve_mib >= 0 && !has_option("--vram-reserve-mib")) {
        arguments.insert(arguments.end(), {"--vram-reserve-mib", std::to_string(vram_reserve_mib)});
    }
}

MemoryPlan ModelCheckpoint::memory_plan(const EngineRelease& release, const std::string& backend, int context,
                                        const std::string& kv_cache, const std::string& experts,
                                        const std::string& kv_streaming, const MemoryCapacity& capacity,
                                        bool custom_arguments) const {
    if (experts != "auto" && experts != "mmap" && experts != "resident") {
        throw std::invalid_argument("strata_experts must be auto, mmap, or resident");
    }
    if (kv_streaming != "auto" && kv_streaming != "on" && kv_streaming != "off") {
        throw std::invalid_argument("strata_kv_streaming must be auto, on, or off");
    }
    MemoryPlan plan;
    if (!custom_arguments && backend != "metal" && capacity.performance_cores > 0 &&
        capacity.efficiency_cores > capacity.performance_cores) {
        const auto workers = static_cast<int64_t>(capacity.performance_cores) - 1 + capacity.efficiency_cores / 2;
        plan.pool_workers = static_cast<int>(std::min<int64_t>(release.pool_workers_cap, std::max<int64_t>(1, workers)));
    }
    if (backend == "metal" || preparation == "native-gguf") {
        if (experts != "auto" || kv_streaming != "auto") {
            throw std::invalid_argument("Explicit Strata memory policy is not supported for this model/backend");
        }
        return plan;
    }
    // Host and GPU allocations compete for the same RAM on integrated GPUs.
    plan.ple_direct = capacity.unified_gpu;
    if (experts == "resident" && profile_file.empty()) {
        throw std::invalid_argument("Strata resident experts require a compatible static expert profile; use auto or mmap");
    }
    plan.experts = experts == "auto" ? (full_expert_arena ? "arena" : "mmap") : experts;
    if (std::isfinite(capacity.total_vram_gib) && capacity.total_vram_gib > 0) {
        const bool small = capacity.total_vram_gib <= release.vram_small_gpu_gib;
        const int floor = small ? release.vram_small_reserve_mib : release.vram_large_reserve_mib;
        const int share = static_cast<int>(std::min(static_cast<double>(std::numeric_limits<int>::max()),
            std::floor(capacity.total_vram_gib * 1024 * release.vram_reserve_fraction)));
        plan.vram_reserve_mib = small || capacity.unified_gpu ? floor : std::max(floor, share);
    }
    if (capacity.wsl && kv_streaming == "on") {
        throw std::invalid_argument("Strata KV streaming is not supported under WSL");
    }
    const int resolved_context = context > 0 ? context : release.default_context;
    const bool stream_candidate = resolved_context >= release.kv_streaming_context && !capacity.wsl;
    if (kv_streaming == "on" && !stream_candidate) {
        throw std::invalid_argument("Strata KV streaming requires context >= " +
            std::to_string(release.kv_streaming_context));
    }
    if (kv_streaming == "on") plan.kv_resident = release.kv_resident_context;
    const auto format = kv_cache == "auto"
        ? (resolved_context <= release.kv_short_context ? release.kv_short_format : release.kv_long_format)
        : kv_cache;
    const auto cell = release.kv_cell_bytes.find(format);
    if (cell == release.kv_cell_bytes.end()) {
        throw std::invalid_argument("Unsupported Strata KV cache format: " + format);
    }
    const double kv_gb = static_cast<double>(resolved_context) * release.kv_layers * cell->second / 1e9;
    if (kv_streaming == "auto" && stream_candidate && !custom_arguments && !capacity.unified_gpu &&
        recommended_ram_gb > 0 && std::isfinite(capacity.total_ram_gib) &&
        capacity.total_ram_gib * 1.073741824 >= recommended_ram_gb + kv_gb + release.kv_streaming_headroom_gb) {
        plan.kv_resident = release.kv_resident_context;
    }
    const bool known_ram = std::isfinite(capacity.total_ram_gib) && capacity.total_ram_gib > 0;
    const double streamed_kv_gib = plan.kv_resident > 0 ? kv_gb / 1.073741824 : 0;
    if (!custom_arguments && experts == "auto" && budget_experts && !capacity.unified_gpu &&
        known_ram && std::isfinite(capacity.expert_size_gib) && capacity.expert_size_gib > 0) {
        if (capacity.total_ram_gib >= capacity.expert_size_gib +
            release.expert_resident_headroom_gib + streamed_kv_gib) {
            plan.experts = "arena";
        } else {
            const double budget = std::floor(std::min(capacity.expert_size_gib,
                capacity.total_ram_gib - release.expert_ram_headroom_gib - std::ceil(streamed_kv_gib)));
            if (budget >= release.expert_min_budget_gib && budget <= std::numeric_limits<int>::max()) {
                plan.experts = "budget";
                plan.resident_budget_gib = static_cast<int>(budget);
            }
        }
    }
    return plan;
}

}

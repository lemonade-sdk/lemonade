#include "lemon/models/scan_catalog.h"

#include "lemon/backends/backend_descriptor_registry.h"
#include "lemon/backends/hf_cache_util.h"
#include "lemon/gguf_reader.h"
#include "lemon/gguf_shard_utils.h"
#include "lemon/hf_variants.h"
#include "lemon/models/model_index.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/safe_fs.h"

#include <algorithm>
#include <fstream>

namespace fs = std::filesystem;

namespace lemon {
namespace models {

ScanCatalog ScanCatalog::for_dir(const std::string& extra_models_dir) {
    DeviceType device = lemon::get_device_type_from_recipe(extra_model_recipe);
    if (const auto* desc = lemon::backends::descriptor_for(extra_model_recipe)) {
        device = desc->default_device;
    }
    return ScanCatalog(extra_models_dir, device);
}

using namespace lemon::utils;

namespace {

// Select the deployment mode from the top-level directory within extra_models_dir.
std::string extra_model_deployment_label(
    const fs::path& model_path,
    const fs::path& search_path) {
    const fs::path relative_path = model_path.lexically_relative(search_path);
    if (relative_path.empty()) return {};

    const auto first_component = relative_path.begin();
    if (first_component == relative_path.end()) return {};

    const std::string category = first_component->string();
    if (category == "chat") return "chat";
    if (category == "embeddings") return "embeddings";
    if (category == "reranking") return "reranking";
    return {};
}

std::string visible_extra_variant_name(const lemon::GgufVariant& variant) {
    std::string stem = fs::path(variant.primary_file).stem().string();
    if (!variant.sharded) {
        return stem;
    }

    const std::vector<std::string> shard_markers = {
        "-00001-of-",
        ".00001-of-",
        "_00001-of-",
    };
    for (const auto& marker : shard_markers) {
        size_t pos = stem.find(marker);
        if (pos != std::string::npos) {
            return stem.substr(0, pos);
        }
    }
    return stem;
}

// <models--org--repo>/snapshots/<commit>/: the folder is an opaque commit hash.
struct CacheRepoNaming {
    std::string name;
    std::string qualifier;
    std::string variant_prefix;
};

static CacheRepoNaming cache_repo_naming(const fs::path& dir_path,
                                         const fs::path& search_path) {
    fs::path snapshot_dir;
    for (fs::path cur = dir_path; cur.has_filename(); cur = cur.parent_path()) {
        if (cur.parent_path().filename() == "snapshots") {
            snapshot_dir = cur;
            break;
        }
        if (cur == search_path) break;
    }
    if (snapshot_dir.empty()) return {};

    const fs::path cache_root = snapshot_dir.parent_path().parent_path();
    const std::string cache_dir_name = cache_root.filename().string();
    std::string encoded;
    for (const std::string prefix : {"modelscope--models--", "models--"}) {
        if (cache_dir_name.rfind(prefix, 0) == 0) {
            encoded = cache_dir_name.substr(prefix.size());
            break;
        }
    }

    // registry_repo_cache_dir_name() encodes "org/repo" as "org--repo". Only the
    // first separator is the org boundary; the rest belongs to the repo name.
    const size_t sep = encoded.find("--");
    const std::string org = sep == std::string::npos ? "" : encoded.substr(0, sep);
    const std::string repo = sep == std::string::npos ? encoded : encoded.substr(sep + 2);
    if (repo.empty()) return {};

    const fs::path live = backends::hf_cache::active_snapshot_path(cache_root);
    if (live.empty()) return {};

    CacheRepoNaming out;
    out.name = dir_path == snapshot_dir ? repo : repo + "-" + dir_path.filename().string();
    if (live != snapshot_dir) {
        // Names the live revision uses stay with it, whatever order the
        // revisions are discovered in.
        out.variant_prefix = snapshot_dir.filename().string() + "-";
        out.name = out.variant_prefix + out.name;
    }
    // Qualifying a collision with the folder would bring back the commit hash.
    out.qualifier = org.empty() ? out.name : org;
    return out;
}

// Record a discovered model without ever overwriting one already found. Two
// extra_models_dir folders can hold identically named files; qualifying the
// newcomer keeps both and leaves the first model's id alone. Ids and kept
// folder names share one namespace, so a qualified id can never shadow a
// folder name another model already owns.
void add_extra_model(std::map<std::string, ModelInfo>& discovered,
                            std::set<std::string>& claimed_names,
                            const std::string& base_name,
                            const std::string& qualifier,
                            ModelInfo info,
                            const std::set<std::string>* reserved_ids = nullptr) {
    static const std::set<std::string> no_reserved;
    const std::string prefix(extra_model_prefix);
    const std::string name = reserve_unique_name(claimed_names,
                                                 reserved_ids ? *reserved_ids : no_reserved,
                                                 prefix, base_name, qualifier);
    info.model_name = prefix + name;
    discovered.emplace(info.model_name, std::move(info));
}

const std::set<std::string>& reserved_extra_model_ids() {
    static const std::set<std::string> ids = {
        std::string(extra_model_prefix) + "chat",
        std::string(extra_model_prefix) + "embeddings",
        std::string(extra_model_prefix) + "reranking",
    };
    return ids;
}

bool is_draft_gguf_filename(std::string filename) {
    std::transform(filename.begin(), filename.end(), filename.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return filename.rfind("mtp-", 0) == 0 ||
           filename.find("-mtp-ud-") != std::string::npos ||
           filename.rfind("dflash-", 0) == 0 || filename == "dflash.gguf";
}

}  // namespace

ModelInfo ScanCatalog::init_info(const std::string& name) const {
    ModelInfo info;
    info.model_name = name;
    info.recipe = extra_model_recipe;
    info.suggested = true;
    info.downloaded = true;
    info.source = extra_model_source;
    info.labels = {"custom"};
    info.device = device_;
    return info;
}

std::map<std::string, ModelInfo> ScanCatalog::enumerate(
    const std::set<fs::path>& registered_paths) const {
    std::map<std::string, ModelInfo> discovered;

    // If no extra models directory configured, return empty
    if (extra_models_dir_.empty()) {
        return discovered;
    }

    fs::path search_path = path_from_utf8(extra_models_dir_).lexically_normal();
    if (!search_path.has_filename()) {
        search_path = search_path.parent_path();
    }
    std::error_code status_ec;
    const fs::file_status status = fs::status(search_path, status_ec);
    if (status_ec) {
        if (status_ec == std::errc::no_such_file_or_directory) {
            return discovered;
        }
        LOG(ERROR, "ModelManager") << "Cannot inspect extra models directory "
                                   << extra_models_dir_ << ": "
                                   << status_ec.message() << std::endl;
        return discovered;
    }
    if (!fs::is_directory(status)) {
        // A missing path is allowed because the directory watcher may observe it
        // later. A non-directory cannot contribute models, but must not affect
        // the registered model cache either.
        return discovered;
    }

    LOG(INFO, "ModelManager") << "Scanning for GGUF models in: " << extra_models_dir_ << std::endl;

    // Track which directories we've processed (for multimodal/multi-shard detection)
    std::map<fs::path, std::vector<fs::path>> dirs_with_gguf;  // directory -> list of gguf files
    std::map<fs::path, std::vector<fs::path>> category_files;
    std::vector<fs::path> standalone_files;  // GGUF files not in subdirectories

    // Recursively find all .gguf files
    try {
        for (const auto& entry : fs::recursive_directory_iterator(
                 search_path, fs::directory_options::skip_permission_denied)) {
            std::error_code entry_ec;
            if (!entry.is_regular_file(entry_ec)) continue;

            std::string filename = entry.path().filename().string();

            if (!gguf_reader_detail::ends_with_ignore_case(filename, ".gguf") ||
                is_draft_gguf_filename(filename)) continue;

            // Registry-owned files were already attached to their original
            // model IDs. Do not surface them again as extra.* models. A
            // directory checkpoint also owns any GGUFs below that directory.
            bool registered = false;
            for (fs::path path = entry.path().lexically_normal(); !path.empty();
                 path = path.parent_path()) {
                if (registered_paths.count(path)) {
                    registered = true;
                    break;
                }
                if (path == search_path) break;
            }
            if (registered) continue;

            fs::path parent_dir = entry.path().parent_path();
            const std::string deployment_label =
                extra_model_deployment_label(entry.path(), search_path);
            const fs::path relative_parent = parent_dir.lexically_relative(search_path);
            const bool directly_in_category =
                !deployment_label.empty() && relative_parent == fs::path(deployment_label);

            if (parent_dir == search_path) {
                standalone_files.push_back(entry.path());
            } else if (directly_in_category) {
                category_files[parent_dir].push_back(entry.path());
            } else {
                dirs_with_gguf[parent_dir].push_back(entry.path());
            }
        }
    } catch (const std::exception& e) {
        LOG(ERROR, "ModelManager") << "Error scanning directory " << extra_models_dir_ << ": " << e.what() << std::endl;
        return discovered;
    }

    // A directory used to be listed as a single model named after itself.
    // Reserving one splits it into separate models, so keep the old id resolving.
    // The first model to keep a folder name owns it in both its bare and
    // extra. forms; resolving each form separately could split them.
    std::set<std::string> claimed_names;
    auto add_standalone_model = [&](const std::vector<fs::path>& model_files,
                                    const std::string& deployment_label,
                                    const fs::path& mmproj_file = fs::path()) {
        const fs::path& gguf_path = model_files.front();
        std::string filename = gguf_path.filename().string();
        std::string shard_base;
        const bool sharded = model_files.size() > 1 &&
            is_gguf_shard_filename(filename, &shard_base);
        const std::string base_name = sharded ? shard_base : gguf_path.stem().string();
        std::string model_name = std::string(extra_model_prefix) + base_name;
        ModelInfo info = init_info(model_name);
        info.checkpoints["main"] = gguf_path.string();
        info.resolved_paths["main"] = gguf_path.string();
        if (!deployment_label.empty()) {
            add_label_once(info.labels, deployment_label);
        }
        lemon::backends::ensure_deployment_label(info.labels, extra_model_recipe);
        info.type = get_model_type_from_labels(info.labels);

        uintmax_t total_size = 0;
        for (const auto& model_file : model_files) {
            try {
                total_size += fs::file_size(model_file);
            } catch (...) {}
        }
        info.size = static_cast<double>(total_size) / (1024.0 * 1024.0 * 1024.0);

        if (!mmproj_file.empty()) {
            info.checkpoints["mmproj"] = mmproj_file.filename().string();
            info.resolved_paths["mmproj"] = mmproj_file.string();
            info.labels.push_back("vision");
        }

        if (!deployment_label.empty() && claimed_names.insert(deployment_label).second) {
            info.input_aliases.push_back(deployment_label);
            info.input_aliases.push_back(std::string(extra_model_prefix) + deployment_label);
        }

        add_extra_model(discovered, claimed_names, base_name,
                        gguf_path.parent_path().filename().string(),
                        std::move(info), deployment_label.empty()
                            ? nullptr
                            : &reserved_extra_model_ids());
    };

    // Files sitting directly in a directory are separate models, except where
    // numbered shard names declare that they belong together.
    auto group_logical_models = [](std::vector<fs::path> files,
                                   std::vector<fs::path>& mmproj_files) {
        std::sort(files.begin(), files.end(),
                  [](const fs::path& lhs, const fs::path& rhs) {
                      return lhs.generic_string() < rhs.generic_string();
                  });
        std::vector<std::vector<fs::path>> logical_models;
        std::map<std::pair<std::string, int>, size_t> shard_groups;

        for (const auto& file : files) {
            const std::string filename = file.filename().string();
            if (gguf_reader_detail::contains_ignore_case(filename, "mmproj")) {
                mmproj_files.push_back(file);
                continue;
            }

            std::string shard_base;
            int shard_total = 0;
            if (is_gguf_shard_filename(filename, &shard_base, &shard_total)) {
                const auto key = std::make_pair(shard_base, shard_total);
                auto [it, inserted] = shard_groups.emplace(key, logical_models.size());
                if (inserted) logical_models.emplace_back();
                logical_models[it->second].push_back(file);
            } else {
                logical_models.push_back({file});
            }
        }

        std::sort(logical_models.begin(), logical_models.end(),
                  [](const auto& lhs, const auto& rhs) {
                      return lhs.front().generic_string() < rhs.front().generic_string();
                  });
        return logical_models;
    };

    // Root files claim their short id first, so adding a reserved directory
    // never renames an extra model that already exists.
    std::vector<fs::path> root_mmproj_files;
    for (const auto& model_files : group_logical_models(standalone_files, root_mmproj_files)) {
        add_standalone_model(model_files, "");
    }

    for (const auto& [category_path, files] : category_files) {
        std::vector<fs::path> mmproj_files;
        const auto logical_models = group_logical_models(files, mmproj_files);

        const std::string deployment_label = category_path.filename().string();
        const fs::path direct_mmproj = logical_models.size() == 1 && !mmproj_files.empty()
            ? mmproj_files.front()
            : fs::path();
        for (const auto& model_files : logical_models) {
            add_standalone_model(model_files, deployment_label, direct_mmproj);
        }
    }

    // Process directories (multimodal and multi-shard models)
    for (auto& [dir_path, gguf_files] : dirs_with_gguf) {
        if (gguf_files.empty()) continue;
        std::sort(gguf_files.begin(), gguf_files.end(),
                  [](const fs::path& lhs, const fs::path& rhs) {
                      return lhs.generic_string() < rhs.generic_string();
                  });
        scan_directory(dir_path, gguf_files, discovered, search_path,
                                           claimed_names);
    }

    LOG(INFO, "ModelManager") << "Discovered " << discovered.size() << " models from extra directory" << std::endl;

    return discovered;
}

void ScanCatalog::scan_directory(
    const fs::path& dir_path,
    const std::vector<fs::path>& gguf_files,
    std::map<std::string, ModelInfo>& discovered,
    const fs::path& search_path,
    std::set<std::string>& claimed_names) const {

    std::string dir_name = dir_path.filename().string();
    std::string qualifier = dir_name;
    std::string folder_qualifier = dir_path.parent_path().filename().string();
    const CacheRepoNaming cache = cache_repo_naming(dir_path, search_path);
    if (!cache.name.empty()) {
        dir_name = cache.name;
        qualifier = folder_qualifier = cache.qualifier;
    }
    const std::string deployment_label = extra_model_deployment_label(dir_path, search_path);
    fs::path main_model_path; // File the old folder-based discovery would have selected.
    std::vector<fs::path> mmproj_files;
    double total_size = 0.0;

    std::vector<std::string> model_filenames;
    std::vector<std::pair<std::string, uint64_t>> model_file_sizes;
    std::map<std::string, fs::path> model_file_by_name;

    for (const auto& gguf_path : gguf_files) {
        uint64_t file_size = 0;
        try {
            file_size = static_cast<uint64_t>(fs::file_size(gguf_path));
            total_size += static_cast<double>(file_size) / (1024.0 * 1024.0 * 1024.0);
        } catch (...) {}

        if (gguf_reader_detail::contains_ignore_case(gguf_path.filename().string(), "mmproj")) {
            mmproj_files.push_back(gguf_path);
            continue;
        }

        std::string filename = gguf_path.filename().string();
        model_filenames.push_back(filename);
        model_file_sizes.emplace_back(filename, file_size);
        model_file_by_name[filename] = gguf_path;

        // Match the old folder behavior: choose the first model file alphabetically.
        if (main_model_path.empty() || gguf_path < main_model_path) {
            main_model_path = gguf_path;
        }
    }

    if (main_model_path.empty()) return;

    std::sort(mmproj_files.begin(), mmproj_files.end());
    fs::path mmproj_file = mmproj_files.empty() ? fs::path() : mmproj_files.front();

    auto vset = lemon::enumerate_gguf_variants(model_filenames, model_file_sizes);

    // Split the folder only when every model file belongs to a named variant.
    // One sharded model stays as the folder model; multiple sharded variants
    // become separate model choices.
    bool should_split = vset.variants.size() > 1;
    for (const auto& v : vset.variants) {
        if (v.name == v.primary_file ||
            model_file_by_name.find(v.primary_file) == model_file_by_name.end()) {
            should_split = false;
            break;
        }
    }

    if (should_split) {
        std::set<std::string> represented_files;
        for (const auto& v : vset.variants) {
            represented_files.insert(v.files.begin(), v.files.end());
        }
        if (represented_files.size() != model_filenames.size()) {
            should_split = false;
        }
    }

    if (should_split) {
        for (const auto& v : vset.variants) {
            auto it = model_file_by_name.find(v.primary_file);
            if (it == model_file_by_name.end()) continue;

            const fs::path& path = it->second;
            const std::string variant_name =
                cache.variant_prefix + visible_extra_variant_name(v);
            std::string variant_id = std::string(extra_model_prefix) + variant_name;

            ModelInfo info = init_info(variant_id);
            info.checkpoints["main"] = path.string();
            info.resolved_paths["main"] = path.string();
            info.size = static_cast<double>(v.size_bytes) / (1024.0 * 1024.0 * 1024.0);

            if (!deployment_label.empty()) {
                add_label_once(info.labels, deployment_label);
            }

            if (!mmproj_file.empty()) {
                info.checkpoints["mmproj"] = mmproj_file.filename().string();
                info.resolved_paths["mmproj"] = mmproj_file.string();
                info.labels.push_back("vision");
            }
            lemon::backends::ensure_deployment_label(info.labels, extra_model_recipe);
            info.type = get_model_type_from_labels(info.labels);

            // Keep the old folder name working in requests without listing it.
            if (path == main_model_path && cache.name.empty() &&
                claimed_names.insert(dir_name).second) {
                info.input_aliases.push_back(dir_name);
                info.input_aliases.push_back(std::string(extra_model_prefix) + dir_name);
            }

            add_extra_model(discovered, claimed_names, variant_name, qualifier,
                            std::move(info), deployment_label.empty()
                                ? nullptr
                                : &reserved_extra_model_ids());
        }
    } else {
        // Keep the folder as one model when splitting would be ambiguous.
        std::string model_id = std::string(extra_model_prefix) + dir_name;
        ModelInfo info = init_info(model_id);
        info.checkpoints["main"] = dir_path.string();
        info.resolved_paths["main"] = main_model_path.string();
        info.size = total_size;

        if (!deployment_label.empty()) {
            add_label_once(info.labels, deployment_label);
        }

        if (!mmproj_file.empty()) {
            info.checkpoints["mmproj"] = mmproj_file.filename().string();
            info.resolved_paths["mmproj"] = mmproj_file.string();
            info.labels.push_back("vision");
        }
        lemon::backends::ensure_deployment_label(info.labels, extra_model_recipe);
        info.type = get_model_type_from_labels(info.labels);
        add_extra_model(discovered, claimed_names, dir_name, folder_qualifier,
                        std::move(info), deployment_label.empty()
                            ? nullptr
                            : &reserved_extra_model_ids());
    }
}

}  // namespace models
}  // namespace lemon

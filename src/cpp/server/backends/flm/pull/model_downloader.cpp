/// \file model_downloader.cpp
/// \brief Model downloader class
/// \author FastFlowLM Team
/// \date 2025-06-24
/// \version 0.9.24
/// \note This class is used to download models from the huggingface
#include "model_downloader.hpp"
#include "utils/utils.hpp"
#include "download_model.hpp"
#include <sstream>
#include <iomanip>

/// \brief Constructor
/// \param models the model list
/// \return the model downloader
ModelDownloader::ModelDownloader(model_list& models)
    : supported_models(models), curl_init() {
}

/// \brief Check if the model is downloaded
/// \param model_tag the model tag
/// \return true if the model is downloaded, false otherwise
bool ModelDownloader::is_model_downloaded(const std::string& model_tag, bool sub_process_mode, bool fast_check) {
    auto missing_files = get_missing_files(model_tag);
    bool is_config_file_missing = std::find(missing_files.begin(), missing_files.end(), "config.json") != missing_files.end();
    if (!is_config_file_missing) {
        if (!check_model_compatibility(model_tag, sub_process_mode)) {
            if (!sub_process_mode)
                header_print("FLM", "Model " + model_tag + " is not compatible with the current FLM version. ");
            // Fast path: skip the expensive HF metadata fetch + per-file hash
            // verification. Caller (e.g. `flm list`) only needs the boolean
            // status; cleanup can happen later on `pull` / `check`.
            if (fast_check) {
                return false;
            }
            // Instead of removing all files wholesale, verify each file against
            // HuggingFace metadata and remove only corrupted ones (same logic
            // as check_model). Files that pass verification can be reused.
            verify_and_clean_files(model_tag, sub_process_mode);
            return false;
        }
    }
    return missing_files.empty();
}

/// \brief Check if the model is compatible with the current FLM version
/// \param model_tag the model tag
/// \return true if the model is compatible, false otherwise
bool ModelDownloader::check_model_compatibility(const std::string& model_tag, bool sub_process_mode) {
    auto [new_model_tag, model_info] = supported_models.get_model_info(model_tag);
    LM_Config config;
    config.from_pretrained(this->supported_models.get_model_path(new_model_tag));
    std::string flm_version = config.flm_version;
    std::string flm_min_version = model_info["flm_min_version"];
    int l_l, m_l, r_l; //left, middle, right on local version
    int l_r, m_r, r_r; //left, middle, right on requried version
    int l_f, m_f, r_f; //left, middle, right on flm version
    sscanf(__FLM_VERSION__, "%d.%d.%d", &l_f, &m_f, &r_f);
    sscanf(flm_version.c_str(), "%d.%d.%d", &l_l, &m_l, &r_l);
    sscanf(flm_min_version.c_str(), "%d.%d.%d", &l_r, &m_r, &r_r);
    uint32_t local_version_u32 = l_l * 1000000 + m_l * 1000 + r_l;
    uint32_t required_version_u32 = l_r * 1000000 + m_r * 1000 + r_r;
    uint32_t flm_version_u32 = l_f * 1000000 + m_f * 1000 + r_f;
    bool is_future_version = false;
    if (local_version_u32 > flm_version_u32) {
        is_future_version = true;
    }
    bool is_compatible = true;
    if (local_version_u32 < required_version_u32) {
        is_compatible = false;
    }
    if (!sub_process_mode) {
        if (is_future_version) {
            header_print("WARNING", "Local model version: " + flm_version + " > " + __FLM_VERSION__);
            header_print("WARNING", "This model may not be compatible with the current FLM version.");
            header_print("WARNING", "Please update FLM to the latest version.");
            exit(0);
        }
        if (!is_compatible) {
            header_print("FLM", "Local model " + model_tag + " version: " + flm_version + " < " + flm_min_version);
            return false;
        }
    }
    return is_compatible;
}
/// \brief Pull the model
/// \param model_tag the model tag
/// \param force_redownload true if the model should be downloaded even if it is already downloaded
/// \return true if the model is downloaded, false otherwise
bool ModelDownloader::pull_model(const std::string& model_tag, bool force_redownload) {
    try {
        // Get model info
        auto [new_model_tag, model_info] = supported_models.get_model_info(model_tag);
        std::string model_name = model_info["name"];
        std::string base_url = model_info["url"];

        header_print("FLM", "Model: " + new_model_tag);
        header_print("FLM", "Name: " + model_name);

        // Check if model is already downloaded
        if (!force_redownload && is_model_downloaded(new_model_tag)) {
            header_print("FLM", "Model already downloaded. Use --force to re-download.");
            return true;
        }

        // If force, verify existing files and remove only corrupted ones,
        // instead of wiping the whole model directory. Files that pass
        // verification will be reused; missing/corrupted ones get re-downloaded.
        if (force_redownload) {
            verify_and_clean_files(new_model_tag);
        }

        // Get missing files
        auto missing_files = get_missing_files(new_model_tag);
        if (missing_files.empty() && !force_redownload) {
            header_print("FLM", "All files already present.");
            return true;
        }

        if (!missing_files.empty()) {
            header_print("FLM", "Missing files (" + std::to_string(missing_files.size()) + "):");
            for (const auto& file : missing_files) {
                std::cout << "  - " << file << std::endl;
            }
        } else {
            header_print("FLM", "All required files are present.");
        }

        // Show present files if any
        auto present_files = get_present_files(new_model_tag);
        if (!present_files.empty()) {
            header_print("FLM", "Present files (" + std::to_string(present_files.size()) + "):");
            for (const auto& file : present_files) {
                std::cout << "  - " << file << std::endl;
            }
        }

        // Build download list
        auto download_list = build_download_list(new_model_tag);
        auto downloads = download_list.first;
        float sum_fize_size = download_list.second;
        if (downloads.empty()) {
            header_print("FLM", "No files to download for model: " + new_model_tag);
            return true; // Return true since all files are already present
        }

        header_print("FLM", "Downloading " + std::to_string(downloads.size()) + " missing files...");

        header_print("FLM", "Files to download (" << std::fixed << std::setprecision(2) << sum_fize_size << " MB): ");
        for (const auto& download : downloads) {
            float file_size = download["size"];
            std::string filename = download["file"];
            std::cout << "  - " << filename << " ("
                << std::fixed << std::setprecision(2) << file_size << " MB)"
                << std::endl;
        }

        // Download files with progress
        bool success = download_utils::download_multiple_files(downloads, get_progress_callback());

        if (success) {
            header_print("FLM", "Model downloaded successfully!");

            // Verify download
            auto final_missing = get_missing_files(new_model_tag);
            if (final_missing.empty()) {
                header_print("FLM", "All files verified successfully.");
            } else {
                header_print("WARNING", "Some files may be missing after download:");
                for (const auto& file : final_missing) {
                    std::cout << "  - " << file << std::endl;
                }
            }
            return true;
        } else {
            header_print("ERROR", "Failed to download model files.");
            return false;
        }

    } catch (const std::exception& e) {
        header_print("ERROR", "Exception during download: " + std::string(e.what()));
        return false;
    }
}

/// \brief Model not found
/// \param model_tag the model tag
void ModelDownloader::model_not_found(const std::string& model_tag) {
    header_print("ERROR", "Model not found: " + model_tag);
    header_print("ERROR", "Supported models: ");
    nlohmann::json models = supported_models.get_all_models();
    for (const auto& model : models["models"]) {
        header_print("ERROR", "  - " + model["name"].get<std::string>());
    }
}

/// \brief Get missing files
/// \param model_tag the model tag
/// \return the missing files
std::vector<std::string> ModelDownloader::get_missing_files(const std::string& model_tag) {
    std::vector<std::string> missing_files;

    try {
        auto [new_model_tag, model_info] = supported_models.get_model_info(model_tag);
        std::string model_name = model_info["name"];
        std::string model_path = supported_models.get_model_path(new_model_tag);
        std::vector<std::string> model_files = model_info["files"];

        // Check if this is a VLM model (default to false if key doesn't exist)

        // Check each required model file
        for (int i = 0; i < model_files.size(); ++i) {
            std::string filename = model_files[i];
            std::string file_path = get_model_file_path(model_path, filename);
            if (!file_exists(file_path)) {
                missing_files.push_back(filename);
            }
        }
    } catch (const std::exception& e) {
        header_print("ERROR", "Error checking missing files: " + std::string(e.what()));
    }

    return missing_files;
}

/// \brief Get present files
/// \param model_tag the model tag
/// \return the present files
std::vector<std::string> ModelDownloader::get_present_files(const std::string& model_tag) {
    std::vector<std::string> present_files;

    try {
        auto [new_model_tag, model_info] = supported_models.get_model_info(model_tag);
        std::string model_name = model_info["name"];
        std::string model_path = supported_models.get_model_path(new_model_tag);
        std::vector<std::string> model_files = model_info["files"];

        // Check if this is a VLM model (default to false if key doesn't exist)

        // Check each required model file
        for (int i = 0; i < model_files.size(); ++i) {
            std::string filename = model_files[i];
            std::string file_path = get_model_file_path(model_path, filename);
            if (file_exists(file_path)) {
                present_files.push_back(filename);
            }
        }
    } catch (const std::exception& e) {
        header_print("ERROR", "Error checking present files: " + std::string(e.what()));
    }

    return present_files;
}

/// \brief Get progress callback
/// \return the progress callback
std::function<void(size_t, size_t)> ModelDownloader::get_progress_callback() {
    return [](size_t completed, size_t total) {
        if (total > 0) {
            double percentage = (static_cast<double>(completed) / total) * 100.0;
            std::cout << "\r[FLM]  Overall progress:  " << completed << "/" << total << " files" << std::flush;

            std::cout << std::endl;
        }
    };
}

/// \brief Check if the file exists
/// \param file_path the file path
/// \return true if the file exists, false otherwise
bool ModelDownloader::file_exists(const std::string& file_path) {
    return std::filesystem::exists(file_path) && std::filesystem::is_regular_file(file_path);
}

/// \brief Get the model file path
/// \param model_path the model path
/// \param filename the filename
/// \return the model file path
std::string ModelDownloader::get_model_file_path(const std::string& model_path, const std::string& filename) {
    std::filesystem::path full_path = std::filesystem::path(model_path) / filename;
    return full_path.string();
}

/// \brief Build the download list
/// \param model_tag the model tag
/// \return the download list
std::pair<nlohmann::json, float> ModelDownloader::build_download_list(const std::string& model_tag) {

    nlohmann::json downloads = nlohmann::json::array();
    float sum_file_size = 0;

    try {
        auto [new_model_tag, model_info] = supported_models.get_model_info(model_tag);
        std::string base_url = model_info["url"];
        std::string model_name = model_info["name"];
        std::string file_url = model_info["file_url"];
        std::vector<std::string> model_files = model_info["files"];

        // Create model directory
        std::string model_path = supported_models.get_model_path(new_model_tag);
        std::filesystem::create_directories(model_path);

        // GET HF api/models
        std::string hf_response = download_utils::download_string(file_url);
        nlohmann::json hf_model_infos = nlohmann::json::parse(hf_response);

        for (const auto& filename : model_files) {
            auto it = std::find_if(
                hf_model_infos.begin(),
                hf_model_infos.end(),
                [&](const nlohmann::json& f) {
                    return f["path"] == filename;
                }
            );
            if (it == hf_model_infos.end()) {
                continue;
            }

            const auto& file = *it;
            std::string local_path = get_model_file_path(model_path, filename);

            if (!file_exists(local_path)) {
                std::string url;
                if (std::string(base_url).find("resolve") != std::string::npos) { // resolve provided , may from a specific branch
                    url = base_url + "/" + filename + "?download=true";
                }
                else {
                    url = base_url + "/resolve/main/" + filename + "?download=true";
                }
                bool is_lfs = file.contains("lfs");
                std::string oid = is_lfs ? file["lfs"]["oid"] : file["oid"];
                float file_size = static_cast<float>(file["size"]) / 1024 / 1024;
                sum_file_size += file_size;

                nlohmann::json entry = {
                    {"file", filename},
                    {"size", file_size},
                    {"url", url},
                    {"localpath", local_path},
                    {"oid", oid},
                    {"is_lfs", is_lfs},
                };
                downloads.push_back(entry);
            }

        }
    }
    catch (const std::exception& e) {
        header_print("ERROR", "Error building download list: " + std::string(e.what()));
    }

    return std::make_pair(downloads, sum_file_size);
}

/// \brief Remove a model and all its files
/// \param model_tag the model tag
/// \return true if the model was successfully removed, false otherwise
bool ModelDownloader::remove_model(const std::string& model_tag, bool sub_process_mode) {
    try {
        // Check if model exists in supported models by trying to get its info
        try {
            supported_models.get_model_info(model_tag);
        } catch (const std::exception& e) {
            header_print("ERROR", "Model not found: " + model_tag);
            model_not_found(model_tag);
            return false;
        }

        // Get model path
        std::string model_path = supported_models.get_model_path(model_tag);

        // Check if model directory exists
        if (!std::filesystem::exists(model_path)) {
            header_print("FLM", "Model directory does not exist: " + model_path);
            return true; // Consider it already removed
        }

        if (!sub_process_mode) {
            header_print("FLM", "Removing model: " + model_tag);
            header_print("FLM", "Path: " + model_path);
        }

        // Remove all files in the model directory
        size_t removed_files = 0;
        for (const auto& entry : std::filesystem::directory_iterator(model_path)) {
            if (entry.is_regular_file()) {
                std::filesystem::remove(entry.path());
                removed_files++;
            }
        }

        // Remove the model directory itself
        if (std::filesystem::remove(model_path)) {
            if(!sub_process_mode)
                header_print("FLM", "Successfully removed " + std::to_string(removed_files) + " files and model directory.");
            return true;
        } else {
            header_print("ERROR", "Failed to remove model directory: " + model_path);
            return false;
        }

    } catch (const std::exception& e) {
        header_print("ERROR", "Exception during model removal: " + std::string(e.what()));
        return false;
    }
}

/// \brief Check hash of model files
/// \param model_tag the model tag
/// \return true if all files are present and compatible, false otherwise
bool ModelDownloader::check_model(const std::string& model_tag, bool sub_process_mode) {
    auto [new_model_tag, model_info] = supported_models.get_model_info(model_tag);
    header_print("FLM", "Checking model: " + new_model_tag + "...\n");

    if (!is_model_downloaded(new_model_tag, sub_process_mode)) {
        header_print("FLM", "Model not exist or not compatible: " + new_model_tag);
        header_print("FLM", "Please use `flm pull " + new_model_tag + "` to download the model.");
        return true;
    }
    else {
        bool ok = verify_and_clean_files(new_model_tag, sub_process_mode);
        if (!ok) {
            header_print("FLM", "Model check completed with errors. Please use `flm pull " + new_model_tag + "` to re-download corrupted files.");
        }
        else {
            header_print("FLM", "Model check completed successfully. All files are present and compatible.");
        }
    }
    return true;
}

/// \brief Verify each model file's hash against HuggingFace metadata and
///        remove any corrupted files. Files that pass verification are kept.
/// \param model_tag the model tag
/// \param sub_process_mode if true, suppress informational logging
/// \return true if all files passed verification, false otherwise
bool ModelDownloader::verify_and_clean_files(const std::string& model_tag, bool sub_process_mode) {
    bool any_error = false;
    try {
        auto [new_model_tag, model_info] = supported_models.get_model_info(model_tag);
        std::vector<std::string> model_files = model_info["files"];
        std::string model_path = supported_models.get_model_path(new_model_tag);
        std::string file_url = model_info["file_url"];

        // GET HF api/models
        std::string hf_response = download_utils::download_string(file_url);
        nlohmann::json hf_model_infos = nlohmann::json::parse(hf_response);

        for (const auto& filename : model_files) {
            if (!sub_process_mode) {
                header_print("FLM", "Checking file: " + filename + "...");
            }

            auto it = std::find_if(
                hf_model_infos.begin(),
                hf_model_infos.end(),
                [&](const nlohmann::json& f) {
                    return f["path"] == filename;
                }
            );
            if (it == hf_model_infos.end()) {
                continue;
            }
            const auto& file = *it;
            std::string local_path = get_model_file_path(model_path, filename);

            // If the file isn't present locally, there's nothing to verify or
            // remove; treat as an error so the caller knows a re-pull is needed.
            if (!file_exists(local_path)) {
                any_error = true;
                header_print("FLM", "File missing: " + filename);
                continue;
            }

            bool is_lfs = file.contains("lfs");
            std::string oid_ref = is_lfs ? file["lfs"]["oid"] : file["oid"];
            std::string local_oid = is_lfs ? download_utils::calculate_file_sha256(local_path) : download_utils::calculate_git_blob_oid(local_path);

            if (local_oid == oid_ref) {
                if (!sub_process_mode) {
                    header_print("FLM", "Success!");
                }
            }
            else {
                if (!sub_process_mode) {
                    header_print("FLM", "Fail!");
                    header_print("FLM", "Removing corrupted file: " + filename + "...");
                }

                if (std::filesystem::remove(local_path)) {
                    if (!sub_process_mode) {
                        header_print("FLM", "Successfully removed " + filename + "!");
                    }
                }
                else {
                    header_print("ERROR", "Failed to remove corrupted file: " + filename);
                }
                any_error = true;
            }
        }
    }
    catch (const std::exception& e) {
        header_print("ERROR", "Exception during file verification: " + std::string(e.what()));
        any_error = true;
    }
    return !any_error;
}

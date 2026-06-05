/// \file model_downloader.hpp
/// \brief Model downloader class
/// \author FastFlowLM Team
/// \date 2025-06-24
/// \version 0.9.24
/// \note This class is used to download models from the huggingface
#pragma once

#include "model_list.hpp"
#include "lm_config.hpp"
#include "download_model.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <vector>
#include <string>
#include <iostream>

class ModelDownloader {
public:
    ModelDownloader(model_list& models);

    // Check if model is already downloaded.
    // When fast_check is true, only the local presence + version compatibility
    // are checked; no HuggingFace metadata is fetched and no per-file hash
    // verification / cleanup is performed. Use this for cheap status queries
    // such as `flm list`.
    bool is_model_downloaded(const std::string& model_tag, bool sub_process_mode=0, bool fast_check=false);

    // Download model files if not present
    bool pull_model(const std::string& model_tag, bool force_redownload = false);

    // Get list of missing files for a model
    std::vector<std::string> get_missing_files(const std::string& model_tag);

    // Get list of present files for a model
    std::vector<std::string> get_present_files(const std::string& model_tag);

    // Remove a model and all its files
    bool remove_model(const std::string& model_tag, bool sub_process_mode=0);

    bool check_model(const std::string& model_tag, bool sub_process_mode=0);

    // Get download progress callback
    std::function<void(size_t, size_t)> get_progress_callback();

    void model_not_found(const std::string& model_tag);

private:
    model_list& supported_models;
    download_utils::CurlInitializer curl_init;

    // Check if a specific file exists
    bool file_exists(const std::string& file_path);

    // Get the full path for a model file
    std::string get_model_file_path(const std::string& model_path, const std::string& filename);

    // Build download URLs for model files
    std::pair<nlohmann::json, float> build_download_list(const std::string& model_tag);

    // bool check_model_compatibility(const std::string& model_tag);
    bool check_model_compatibility(const std::string& model_tag, bool sub_process_mode=0);

    // Verify per-file integrity against HuggingFace metadata and remove any
    // corrupted files. Returns true if all files passed verification.
    bool verify_and_clean_files(const std::string& model_tag, bool sub_process_mode=0);
};

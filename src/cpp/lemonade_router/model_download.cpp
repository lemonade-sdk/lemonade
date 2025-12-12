// Model download functions for lemon::ModelManager
// These are ModelManager methods extracted to a separate file for organization.
// Functions: download_model, download_from_huggingface, download_from_flm

#include <lemon/model_manager.h>
#include <lemon/utils/http_client.h>
#include <lemon/utils/process_manager.h>
#include <lemon/backends/fastflowlm_server.h>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <thread>
#include <chrono>
#include <unordered_set>
#include <iomanip>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {

// Helper functions for string operations (duplicated for file-local use)
static std::string to_lower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(), ::tolower);
    return result;
}

static bool ends_with_ignore_case(const std::string& str, const std::string& suffix) {
    if (suffix.length() > str.length()) {
        return false;
    }
    return to_lower(str.substr(str.length() - suffix.length())) == to_lower(suffix);
}

static bool starts_with_ignore_case(const std::string& str, const std::string& prefix) {
    if (prefix.length() > str.length()) {
        return false;
    }
    return to_lower(str.substr(0, prefix.length())) == to_lower(prefix);
}

static bool contains_ignore_case(const std::string& str, const std::string& substr) {
    return to_lower(str).find(to_lower(substr)) != std::string::npos;
}

// Structure to hold identified GGUF files
struct GGUFFiles {
    std::map<std::string, std::string> core_files;  // {"variant": "file.gguf", "mmproj": "file.mmproj"}
    std::vector<std::string> sharded_files;         // Additional shard files
};

// Identifies GGUF model files matching the variant
static GGUFFiles identify_gguf_models(
    const std::string& checkpoint,
    const std::string& variant,
    const std::string& mmproj,
    const std::vector<std::string>& repo_files
) {
    const std::string hint = R"(
    The CHECKPOINT:VARIANT scheme is used to specify model files in Hugging Face repositories.

    The VARIANT format can be one of several types:
    0. wildcard (*): download all .gguf files in the repo
    1. Full filename: exact file to download
    2. None/empty: gets the first .gguf file in the repository (excludes mmproj files)
    3. Quantization variant: find a single file ending with the variant name (case insensitive)
    4. Folder name: downloads all .gguf files in the folder that matches the variant name (case insensitive)
    )";

    GGUFFiles result;
    std::vector<std::string> sharded_files;
    std::string variant_name;

    // (case 0) Wildcard, download everything
    if (!variant.empty() && variant == "*") {
        for (const auto& f : repo_files) {
            if (ends_with_ignore_case(f, ".gguf")) {
                sharded_files.push_back(f);
            }
        }
        
        if (sharded_files.empty()) {
            throw std::runtime_error("No .gguf files found in repository " + checkpoint + ". " + hint);
        }
        
        std::sort(sharded_files.begin(), sharded_files.end());
        variant_name = sharded_files[0];
    }
    // (case 1) If variant ends in .gguf or .bin, use it directly
    else if (!variant.empty() && (ends_with_ignore_case(variant, ".gguf") || ends_with_ignore_case(variant, ".bin"))) {
        variant_name = variant;

        bool found = false;
        for (const auto& f : repo_files) {
            if (f == variant) {
                found = true;
                break;
            }
        }

        if (!found) {
            throw std::runtime_error(
                "File " + variant + " not found in Hugging Face repository " + checkpoint + ". " + hint
            );
        }
    }
    // (case 2) If no variant is provided, get the first .gguf file
    else if (variant.empty()) {
        std::vector<std::string> all_variants;
        for (const auto& f : repo_files) {
            if (ends_with_ignore_case(f, ".gguf") && !contains_ignore_case(f, "mmproj")) {
                all_variants.push_back(f);
            }
        }
        
        if (all_variants.empty()) {
            throw std::runtime_error(
                "No .gguf files found in Hugging Face repository " + checkpoint + ". " + hint
            );
        }
        
        variant_name = all_variants[0];
    }
    else {
        // (case 3) Find a single file ending with the variant name
        std::vector<std::string> end_with_variant;
        std::string variant_suffix = variant + ".gguf";
        
        for (const auto& f : repo_files) {
            if (ends_with_ignore_case(f, variant_suffix) && !contains_ignore_case(f, "mmproj")) {
                end_with_variant.push_back(f);
            }
        }
        
        if (end_with_variant.size() == 1) {
            variant_name = end_with_variant[0];
        }
        else if (end_with_variant.size() > 1) {
            throw std::runtime_error(
                "Multiple .gguf files found for variant " + variant + ", but only one is allowed. " + hint
            );
        }
        // (case 4) Check for folder with sharded files
        else {
            std::string folder_prefix = variant + "/";
            for (const auto& f : repo_files) {
                if (ends_with_ignore_case(f, ".gguf") && starts_with_ignore_case(f, folder_prefix)) {
                    sharded_files.push_back(f);
                }
            }
            
            if (sharded_files.empty()) {
                throw std::runtime_error(
                    "No .gguf files found for variant " + variant + ". " + hint
                );
            }
            
            std::sort(sharded_files.begin(), sharded_files.end());
            variant_name = sharded_files[0];
        }
    }
    
    result.core_files["variant"] = variant_name;
    result.sharded_files = sharded_files;
    
    // Handle mmproj file
    if (!mmproj.empty()) {
        bool found = false;
        for (const auto& f : repo_files) {
            if (f == mmproj) {
                found = true;
                break;
            }
        }
        
        if (!found) {
            throw std::runtime_error(
                "The provided mmproj file " + mmproj + " was not found in " + checkpoint + "."
            );
        }
        
        result.core_files["mmproj"] = mmproj;
    }
    
    return result;
}

void ModelManager::download_model(const std::string& model_name,
                                 const std::string& checkpoint,
                                 const std::string& recipe,
                                 bool reasoning,
                                 bool vision,
                                 bool embedding,
                                 bool reranking,
                                 const std::string& mmproj,
                                 bool do_not_upgrade,
                                 DownloadProgressCallback progress_callback) {
    
    std::string actual_checkpoint = checkpoint;
    std::string actual_recipe = recipe;
    std::string actual_mmproj = mmproj;
    
    // Check if model exists in registry
    bool model_registered = model_exists(model_name);
    
    if (!model_registered) {
        // Model not in registry - this must be a user model registration
        if (model_name.substr(0, 5) != "user.") {
            throw std::runtime_error(
                "When registering a new model, the model name must include the "
                "`user` namespace, for example `user.Phi-4-Mini-GGUF`. Received: " + 
                model_name
            );
        }
        
        if (actual_checkpoint.empty() || actual_recipe.empty()) {
            throw std::runtime_error(
                "Model " + model_name + " is not registered with Lemonade Server. "
                "To register and install it, provide the `checkpoint` and `recipe` "
                "arguments, as well as the optional `reasoning` and `mmproj` arguments "
                "as appropriate."
            );
        }
        
        // Validate GGUF models require a variant
        if (actual_recipe == "llamacpp") {
            std::string checkpoint_lower = actual_checkpoint;
            std::transform(checkpoint_lower.begin(), checkpoint_lower.end(), 
                          checkpoint_lower.begin(), ::tolower);
            if (checkpoint_lower.find("gguf") != std::string::npos && 
                actual_checkpoint.find(':') == std::string::npos) {
                throw std::runtime_error(
                    "You are required to provide a 'variant' in the checkpoint field when "
                    "registering a GGUF model. The variant is provided as CHECKPOINT:VARIANT. "
                    "For example: Qwen/Qwen2.5-Coder-3B-Instruct-GGUF:Q4_0 or "
                    "Qwen/Qwen2.5-Coder-3B-Instruct-GGUF:qwen2.5-coder-3b-instruct-q4_0.gguf"
                );
            }
        }
        
        std::cout << "Registering new user model: " << model_name << std::endl;
    } else {
        // Model is registered - look up checkpoint if not provided
        if (actual_checkpoint.empty()) {
            auto info = get_model_info(model_name);
            actual_checkpoint = info.checkpoint;
            actual_recipe = info.recipe;
        }
        
        // Look up mmproj if not provided (for vision models)
        if (actual_mmproj.empty()) {
            auto info = get_model_info(model_name);
            actual_mmproj = info.mmproj;
            if (!actual_mmproj.empty()) {
                std::cout << "[ModelManager] Found mmproj for vision model: " << actual_mmproj << std::endl;
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
    
    std::cout << "Downloading model: " << repo_id;
    if (!variant.empty()) {
        std::cout << " (variant: " << variant << ")";
    }
    std::cout << std::endl;
    
    // Check if offline mode
    const char* offline_env = std::getenv("LEMONADE_OFFLINE");
    if (offline_env && std::string(offline_env) == "1") {
        std::cout << "Offline mode enabled, skipping download" << std::endl;
        return;
    }
    
    // Skip if already downloaded and do_not_upgrade is set
    if (do_not_upgrade && is_model_downloaded(model_name)) {
        std::cout << "[ModelManager] Model already downloaded and do_not_upgrade=true, using cached version" << std::endl;
        return;
    }
    
    // Use FLM pull for FLM models, otherwise download from HuggingFace
    if (actual_recipe == "flm") {
        download_from_flm(actual_checkpoint, do_not_upgrade, progress_callback);
    } else if (actual_recipe == "llamacpp" || actual_recipe == "whispercpp") {
        download_from_huggingface(repo_id, variant, actual_mmproj, progress_callback);
    } else {
        download_from_huggingface(repo_id, "", "", progress_callback);
    }
    
    // Register if needed
    if (model_name.substr(0, 5) == "user." || !checkpoint.empty()) {
        register_user_model(model_name, actual_checkpoint, actual_recipe, 
                          reasoning, vision, embedding, reranking, actual_mmproj);
    }
    
    // Update cache after successful download
    update_model_in_cache(model_name, true);
}

void ModelManager::download_from_huggingface(const std::string& repo_id,
                                            const std::string& variant,
                                            const std::string& mmproj,
                                            DownloadProgressCallback progress_callback) {
    // Get Hugging Face cache directory
    std::string hf_cache = get_hf_cache_dir();
    
    // Construct snapshot path
    std::string repo_cache_name = repo_id;
    std::replace(repo_cache_name.begin(), repo_cache_name.end(), '/', '-');
    std::replace(repo_cache_name.begin(), repo_cache_name.end(), '-', '-');
    std::string snapshot_path = hf_cache + "/models--" + repo_cache_name;
    
    std::cout << "[ModelManager] HuggingFace cache: " << hf_cache << std::endl;
    std::cout << "[ModelManager] Snapshot path: " << snapshot_path << std::endl;
    
    try {
        // Get file list from HuggingFace API
        std::string api_url = "https://huggingface.co/api/models/" + repo_id;
        std::cout << "[ModelManager] Fetching model info from: " << api_url << std::endl;
        
        auto response = HttpClient::get(api_url);
        if (response.status_code != 200) {
            throw std::runtime_error("Failed to fetch model info from HuggingFace: HTTP " + std::to_string(response.status_code));
        }
        
        auto model_info = nlohmann::json::parse(response.body);
        
        // Extract file list
        std::vector<std::string> repo_files;
        if (model_info.contains("siblings")) {
            for (const auto& sibling : model_info["siblings"]) {
                if (sibling.contains("rfilename")) {
                    repo_files.push_back(sibling["rfilename"].get<std::string>());
                }
            }
        }
        
        if (repo_files.empty()) {
            throw std::runtime_error("No files found in repository: " + repo_id);
        }
        
        std::cout << "[ModelManager] Found " << repo_files.size() << " files in repository" << std::endl;
        
        // Determine which files to download
        std::vector<std::string> files_to_download;
        
        // Check if this is a GGUF model (llamacpp)
        bool is_gguf = false;
        for (const auto& f : repo_files) {
            if (ends_with_ignore_case(f, ".gguf")) {
                is_gguf = true;
                break;
            }
        }
        
        if (is_gguf && !variant.empty()) {
            // Use GGUF-specific file selection
            auto gguf_files = identify_gguf_models(repo_id, variant, mmproj, repo_files);
            
            // Add the main variant file
            if (gguf_files.core_files.count("variant")) {
                files_to_download.push_back(gguf_files.core_files["variant"]);
            }
            
            // Add mmproj if present
            if (gguf_files.core_files.count("mmproj")) {
                files_to_download.push_back(gguf_files.core_files["mmproj"]);
            }
            
            // Add any sharded files
            for (const auto& shard : gguf_files.sharded_files) {
                if (std::find(files_to_download.begin(), files_to_download.end(), shard) == files_to_download.end()) {
                    files_to_download.push_back(shard);
                }
            }
        } else if (is_gguf) {
            // GGUF without variant - get first .gguf file
            for (const auto& f : repo_files) {
                if (ends_with_ignore_case(f, ".gguf") && !contains_ignore_case(f, "mmproj")) {
                    files_to_download.push_back(f);
                    break;
                }
            }
            
            // Add mmproj if specified
            if (!mmproj.empty()) {
                files_to_download.push_back(mmproj);
            }
        } else {
            // Non-GGUF model - download all files
            files_to_download = repo_files;
        }
        
        std::cout << "[ModelManager] Files to download: " << files_to_download.size() << std::endl;
        for (const auto& f : files_to_download) {
            std::cout << "[ModelManager]   - " << f << std::endl;
        }
        
        // Create snapshot directory
        fs::create_directories(snapshot_path);
        
        // Download each file
        int file_index = 0;
        size_t total_files = files_to_download.size();
        
        for (const auto& filename : files_to_download) {
            file_index++;
            
            std::string file_path = snapshot_path + "/" + filename;
            
            // Create parent directories if needed
            fs::path parent_dir = fs::path(file_path).parent_path();
            fs::create_directories(parent_dir);
            
            // Check if file already exists
            if (fs::exists(file_path)) {
                std::cout << "[ModelManager] File already exists, skipping: " << filename << std::endl;
                
                // Still send progress callback
                if (progress_callback) {
                    DownloadProgress progress;
                    progress.file = filename;
                    progress.file_index = file_index;
                    progress.total_files = total_files;
                    progress.bytes_downloaded = fs::file_size(file_path);
                    progress.bytes_total = fs::file_size(file_path);
                    progress.percent = 100;
                    progress.complete = (file_index == (int)total_files);
                    
                    if (!progress_callback(progress)) {
                        throw std::runtime_error("Download cancelled");
                    }
                }
                continue;
            }
            
            // Download file
            std::string download_url = "https://huggingface.co/" + repo_id + "/resolve/main/" + filename;
            std::cout << "[ModelManager] Downloading: " << filename << std::endl;
            
            // Use streaming download with progress
            auto download_result = HttpClient::download_file(
                download_url, 
                file_path,
                [&](size_t bytes_downloaded, size_t bytes_total) -> bool {
                    if (progress_callback) {
                        DownloadProgress progress;
                        progress.file = filename;
                        progress.file_index = file_index;
                        progress.total_files = total_files;
                        progress.bytes_downloaded = bytes_downloaded;
                        progress.bytes_total = bytes_total;
                        progress.percent = (bytes_total > 0) ? 
                            (int)((bytes_downloaded * 100) / bytes_total) : 0;
                        progress.complete = false;
                        
                        return progress_callback(progress);
                    }
                    return true;
                }
            );
            
            if (!download_result.success) {
                throw std::runtime_error("Failed to download " + filename + ": " + download_result.error_message);
            }
            
            std::cout << "[ModelManager] Downloaded: " << filename << std::endl;
        }
        
        // Send final complete event
        if (progress_callback) {
            DownloadProgress progress;
            progress.complete = true;
            progress.file_index = total_files;
            progress.total_files = total_files;
            progress.percent = 100;
            (void)progress_callback(progress);
        }
        
        std::cout << "[ModelManager] ✓ All files downloaded and validated successfully!" << std::endl;
        std::cout << "[ModelManager DEBUG] Download location: " << snapshot_path << std::endl;
        
    } catch (const std::exception& e) {
        throw;
    }
}

void ModelManager::download_from_flm(const std::string& checkpoint, 
                                     bool do_not_upgrade,
                                     DownloadProgressCallback progress_callback) {
    std::cout << "[ModelManager] Pulling FLM model: " << checkpoint << std::endl;
    
    // Ensure FLM is installed
    std::cout << "[ModelManager] Checking FLM installation..." << std::endl;
    backends::FastFlowLMServer flm_installer("info", this);
    try {
        flm_installer.install();
    } catch (const std::exception& e) {
        std::cerr << "[ModelManager ERROR] FLM installation failed: " << e.what() << std::endl;
        throw;
    }
    
    // Find flm executable
    std::string flm_path = "flm";
    
    // Prepare arguments
    std::vector<std::string> args = {"pull", checkpoint};
    if (!do_not_upgrade) {
        args.push_back("--force");
    }
    
    std::cout << "[ProcessManager] Starting process: \"" << flm_path << "\"";
    for (const auto& arg : args) {
        std::cout << " \"" << arg << "\"";
    }
    std::cout << std::endl;
    
    // State for parsing FLM output
    int total_files = 0;
    int current_file_index = 0;
    std::string current_filename;
    bool cancelled = false;
    
    // Run flm pull command and parse output
    int exit_code = utils::ProcessManager::run_process_with_output(
        flm_path, args,
        [&](const std::string& line) -> bool {
            std::cout << line << std::endl;
            
            // Parse FLM output for progress
            if (line.find("[FLM]  Downloading ") != std::string::npos && 
                line.find("/") != std::string::npos && 
                line.find(":") != std::string::npos) {
                
                size_t start = line.find("Downloading ") + 12;
                size_t slash = line.find("/", start);
                size_t colon = line.find(":", slash);
                
                if (slash != std::string::npos && colon != std::string::npos) {
                    try {
                        current_file_index = std::stoi(line.substr(start, slash - start));
                        total_files = std::stoi(line.substr(slash + 1, colon - slash - 1));
                        current_filename = line.substr(colon + 2);
                        
                        if (progress_callback) {
                            DownloadProgress progress;
                            progress.file = current_filename;
                            progress.file_index = current_file_index;
                            progress.total_files = total_files;
                            progress.bytes_downloaded = 0;
                            progress.bytes_total = 0;
                            progress.percent = (total_files > 0) ? 
                                ((current_file_index - 1) * 100 / total_files) : 0;
                            
                            if (!progress_callback(progress)) {
                                cancelled = true;
                                return false;
                            }
                        }
                    } catch (...) {
                        // Ignore parse errors
                    }
                }
            }
            else if (line.find("[FLM]  Downloading: ") != std::string::npos && 
                     line.find("%") != std::string::npos) {
                
                size_t start = line.find("Downloading: ") + 13;
                size_t pct_end = line.find("%", start);
                
                if (pct_end != std::string::npos) {
                    try {
                        std::string pct_str = line.substr(start, pct_end - start);
                        double file_percent = std::stod(pct_str);
                        
                        size_t bytes_downloaded = 0;
                        size_t bytes_total = 0;
                        
                        size_t open_paren = line.find("(", pct_end);
                        size_t slash = line.find("/", open_paren);
                        size_t close_paren = line.find(")", slash);
                        
                        if (open_paren != std::string::npos && slash != std::string::npos) {
                            std::string downloaded_str = line.substr(open_paren + 1, slash - open_paren - 1);
                            std::string total_str = line.substr(slash + 1, close_paren - slash - 1);
                            
                            auto parse_size = [](const std::string& s) -> size_t {
                                double val = 0;
                                size_t mb_pos = s.find("MB");
                                size_t gb_pos = s.find("GB");
                                
                                if (mb_pos != std::string::npos) {
                                    val = std::stod(s.substr(0, mb_pos)) * 1024 * 1024;
                                } else if (gb_pos != std::string::npos) {
                                    val = std::stod(s.substr(0, gb_pos)) * 1024 * 1024 * 1024;
                                } else {
                                    val = std::stod(s);
                                }
                                return static_cast<size_t>(val);
                            };
                            
                            try {
                                bytes_downloaded = parse_size(downloaded_str);
                                bytes_total = parse_size(total_str);
                            } catch (...) {}
                        }
                        
                        if (progress_callback) {
                            DownloadProgress progress;
                            progress.file = current_filename;
                            progress.file_index = current_file_index;
                            progress.total_files = total_files;
                            progress.bytes_downloaded = bytes_downloaded;
                            progress.bytes_total = bytes_total;
                            
                            double overall_percent = 0;
                            if (total_files > 0) {
                                double file_contribution = 100.0 / total_files;
                                double completed_files_percent = (current_file_index - 1) * file_contribution;
                                double current_file_percent = (file_percent / 100.0) * file_contribution;
                                overall_percent = completed_files_percent + current_file_percent;
                            }
                            progress.percent = static_cast<int>(overall_percent);
                            
                            if (!progress_callback(progress)) {
                                cancelled = true;
                                return false;
                            }
                        }
                    } catch (...) {}
                }
            }
            
            return !cancelled;
        }
    );
    
    if (cancelled) {
        throw std::runtime_error("Download cancelled");
    }
    
    if (exit_code != 0) {
        throw std::runtime_error("FLM pull failed with exit code " + std::to_string(exit_code));
    }
    
    // Send final complete event
    if (progress_callback) {
        DownloadProgress progress;
        progress.complete = true;
        progress.file_index = total_files;
        progress.total_files = total_files;
        progress.percent = 100;
        (void)progress_callback(progress);
    }
    
    std::cout << "[ModelManager] FLM model pull completed successfully" << std::endl;
}

} // namespace lemon


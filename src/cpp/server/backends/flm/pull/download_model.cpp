/// \file download_model.cpp
/// \brief Download model class
/// \author FastFlowLM Team
/// \date 2025-06-24
/// \version 0.9.24
/// \note This class for curl download
#include "download_model.hpp"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <iomanip>
#include "utils/utils.hpp"
#include <nlohmann/json.hpp>
#include "picosha2.h"
#include "sha1.hpp"

namespace download_utils {

/// \brief Calculates the SHA256 hash of a file.
/// \param file_path The path to the file.
/// \return A string representing the hex digest of the hash, or an empty string on error.
std::string calculate_file_sha256(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        return "";
    }

    std::vector<unsigned char> hash(picosha2::k_digest_size);
    picosha2::hash256(file, hash.begin(), hash.end());
    return picosha2::bytes_to_hex_string(hash.begin(), hash.end());
}

std::string calculate_git_blob_oid(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        return "";
    }
    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::ostringstream oss;
    oss << "blob " << size << '\0';  // Git blob header
    oss << file.rdbuf();

    std::string blob_data = oss.str();
    SHA1 sha1;
    sha1.update(blob_data);
    return sha1.final();
}

// Global variable to track if progress bar was shown
static bool g_progress_bar_shown = false;

/// \brief Hide the cursor
void hide_cursor() {
    std::cout << "\033[?25l" << std::flush;
}

/// \brief Show the cursor
void show_cursor() {
    std::cout << "\033[?25h" << std::flush;
}

/// \brief Callback function for libcurl to write data to a file
/// \param ptr the pointer to the data
/// \param size the size of the data
/// \param nmemb the number of items
/// \param stream the stream to write to
/// \return the number of bytes written
size_t write_data_to_file(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    size_t written = fwrite(ptr, size, nmemb, stream);
    return written;
}

/// \brief Callback function for libcurl to write data to a string
/// \param ptr the pointer to the data
/// \param size the size of the data
/// \param nmemb the number of items
/// \param userdata the string to write to
/// \return the number of bytes written
size_t write_data_to_string(void* ptr, size_t size, size_t nmemb, std::string* userdata) {
    userdata->append((char*)ptr, size * nmemb);
    return size * nmemb;
}

/// \brief Progress callback function
/// \param clientp the client pointer
/// \param dltotal the total download size
/// \param dlnow the current download size
/// \param ultotal the total upload size
/// \param ulnow the current upload size
/// \return the progress
int progress_callback(void* clientp, double dltotal, double dlnow, double ultotal, double ulnow) {
    if (dltotal > 0) {
        using Clock = std::chrono::steady_clock;
        static Clock::time_point last_print_time = Clock::now();

        auto now = Clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_print_time);

        double percentage = (dlnow / dltotal) * 100.0;

        if (elapsed.count() >= 1000) {
            utils::enable_ansi_on_windows_once();

            double percentage = (dlnow / dltotal) * 100.0;
            double mb_now = dlnow / 1024.0 / 1024.0;
            double mb_total = dltotal / 1024.0 / 1024.0;

            std::cout << "\r\033[K"
                << "[FLM]  Downloading: " << std::fixed << std::setprecision(1)
                << percentage << "% (" << mb_now << "MB / " << mb_total << "MB)"
                << std::flush;

            g_progress_bar_shown = true;
            last_print_time = now;
        }
    }
    return 0;
}

/// \brief Download a file from URL to a local file
/// \param url the URL to download from
/// \param local_path the local path to save the file
/// \param progress_cb the progress callback
/// \return true if the file is downloaded, false otherwise
bool download_file(const std::string& url, const std::string& local_path, bool is_lfs, std::string remote_oid,
                   std::function<void(double)> progress_cb) {
    // Reset progress bar tracking for this download
    g_progress_bar_shown = false;

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "Failed to initialize CURL" << std::endl;
        return false;
    }

    // Create directory if it doesn't exist
    std::filesystem::path path(local_path);
    std::filesystem::create_directories(path.parent_path());

    FILE* fp = fopen(local_path.c_str(), "wb");
    if (!fp) {
        std::cerr << "Failed to open file for writing: " << local_path << std::endl;
        curl_easy_cleanup(curl);
        return false;
    }

    // Hide cursor before starting download
    hide_cursor();

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data_to_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "FastFlowLM/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3600L); // 1 hour timeout

    // Set progress callback if provided
    if (progress_cb) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, progress_callback);
    }

    CURLcode res = curl_easy_perform(curl);

    fclose(fp);
    curl_easy_cleanup(curl);

    // Show cursor after download completes
    show_cursor();

    if (res != CURLE_OK) {
        std::cerr << "CURL error: " << curl_easy_strerror(res) << std::endl;
        std::filesystem::remove(local_path); // Remove partial download
        return false;
    }

    // Only add newline if progress bar was shown
    if (g_progress_bar_shown) {
        std::cout << std::endl;
    }

    header_print("FLM", "Checking Hash...");
    std::string local_oid = is_lfs ? calculate_file_sha256(local_path) : calculate_git_blob_oid(local_path);
    if (local_oid != remote_oid) {
        header_print("FLM", "Hash not matched!");
        show_cursor(); // Show cursor on error
        return false;
    }

    header_print("FLM", "Download completed: " << local_path);
    return true;
}

static bool download_with_retry(const std::string& url, const std::string& local_path, bool is_lfs, std::string remote_oid,
    std::function<void(double)> progress_cb, int max_retries = 3) {
    int attempt = 0;
    while (attempt < max_retries) {
        if (download_file(url, local_path, is_lfs, remote_oid, progress_cb)) {
            return true;
        }
        header_print("FLM", "Download failed (attempt " << (attempt + 1) << "/" << max_retries << ")");
        if(attempt < max_retries - 1)
            header_print("FLM", "Retrying...");
        attempt++;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return false;
}

/// \brief Download content from URL to a string
/// \param url the URL to download from
/// \return the downloaded string
std::string download_string(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "Failed to initialize CURL" << std::endl;
        return "";
    }

    std::string response;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "FastFlowLM/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L); // 1 minute timeout

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        std::cerr << "CURL error: " << curl_easy_strerror(res) << std::endl;
        return "";
    }

    return response;
}

/// \brief Download multiple files with progress tracking
/// \param downloads the downloads
/// \param progress_cb the progress callback
/// \return true if the files are downloaded, false otherwise
bool download_multiple_files(const nlohmann::json downloads,
                           std::function<void(size_t, size_t)> progress_cb) {
    size_t total_files = downloads.size();
    size_t completed_files = 0;

    // Hide cursor before starting downloads
    //hide_cursor();

    for (auto& file : downloads) {
        std::string url = file["url"];
        std::string local_path = file["localpath"];
        std::string filename = std::filesystem::path(url).filename().string();
        std::string remote_oid = file["oid"];
        bool is_lfs = file["is_lfs"];

        // cut "?download=true"
        if (filename.find("?download=true") != std::string::npos) {
            filename = filename.substr(0, filename.find("?download=true"));
        }
        header_print("FLM", "Downloading " << (completed_files + 1) << "/" << total_files
                  << ": " << filename);

        auto file_progress = [&](double percentage) {
            if (progress_cb) {
                progress_cb(completed_files, total_files);
            }
        };

        if (!download_with_retry(url, local_path, is_lfs, remote_oid, file_progress)) {
            std::cerr << "Failed to download: " << url << std::endl;
            //show_cursor(); // Show cursor on error
            return false;
        }

        completed_files++;
        if (progress_cb) {
            progress_cb(completed_files, total_files);
        }
    }

    // Show cursor after all downloads complete
    show_cursor();
    header_print("FLM", "All downloads completed successfully!");
    return true;
}

/// \brief Initialize CURL library (call this once at program startup)
/// \return true if the CURL library is initialized, false otherwise
bool init_curl() {
    CURLcode res = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (res != CURLE_OK) {
        std::cerr << "Failed to initialize CURL library: " << curl_easy_strerror(res) << std::endl;
        return false;
    }
    return true;
}

/// \brief Cleanup CURL library (call this once at program shutdown)
void cleanup_curl() {
    curl_global_cleanup();
}

/// \brief RAII wrapper for CURL initialization
/// \return the CURL initializer
CurlInitializer::CurlInitializer() {
    if (!init_curl()) {
        throw std::runtime_error("Failed to initialize CURL");
    }
}

/// \brief Destructor
CurlInitializer::~CurlInitializer() {
    cleanup_curl();
}

} // namespace download_utils

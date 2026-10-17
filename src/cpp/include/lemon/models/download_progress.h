#pragma once

#include <functional>
#include <string>

namespace lemon {

// Progress information for download operations
struct DownloadProgress {
    std::string file;           // Current file being downloaded
    int file_index = 0;         // Current file index (1-based)
    int total_files = 0;        // Total files in this download
    size_t bytes_downloaded = 0; // Bytes downloaded for current file
    size_t bytes_total = 0;     // Total bytes for current file
    size_t total_download_size = 0; // Total bytes across ALL files in this download
    size_t bytes_previously_downloaded = 0; // Bytes already on disk (resume offset or skipped file)
    int percent = 0;            // Overall percentage (0-100)
    bool complete = false;      // True when all downloads finished
    std::string error;          // Error message if failed
};

// Callback for download progress updates
// Returns bool: true = continue download, false = cancel download
using DownloadProgressCallback = std::function<bool(const DownloadProgress&)>;

}  // namespace lemon

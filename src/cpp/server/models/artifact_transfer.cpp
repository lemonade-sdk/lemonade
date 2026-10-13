#include <lemon/gguf_reader.h>
#include <lemon/models/artifact_transfer.h>
#include <lemon/utils/aixlog.hpp>
#include <lemon/utils/http_client.h>
#include <lemon/utils/path_utils.h>
#include <lemon/utils/safe_fs.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <tuple>

namespace fs = std::filesystem;
using namespace lemon::utils;

namespace lemon {
namespace models {

namespace {

constexpr const char* kFileScheme = "file://";

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

// A file URL for an absolute Windows path adds a third slash before the drive
// letter. Percent-encoding is not decoded: nothing mints a file URL with an
// encoded character today.
std::string path_from_file_url(const std::string& url) {
    std::string rest = url.substr(std::strlen(kFileScheme));
    if (rest.size() >= 3 && rest[0] == '/' &&
        std::isalpha(static_cast<unsigned char>(rest[1])) && rest[2] == ':') {
        rest.erase(rest.begin());
    }
    return rest;
}

std::string size_in_gb(std::uint64_t bytes) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << (bytes / (1024.0 * 1024.0 * 1024.0));
    return oss.str();
}

}  // namespace

DownloadPlan download_plan_from_manifest(const nlohmann::json& manifest) {
    DownloadPlan plan;
    plan.download_path = manifest.at("download_path").get<std::string>();

    for (const auto& file_desc : manifest.at("files")) {
        DownloadFile file;
        file.name = file_desc.at("name").get<std::string>();
        file.url = file_desc.at("url").get<std::string>();
        file.size = file_desc.at("size").get<std::uint64_t>();
        // Per-file target for multi-repository plans.
        file.download_path = file_desc.value("download_path", std::string());

        if (file_desc.contains("hash") && file_desc["hash"].is_object()) {
            const auto& hash = file_desc["hash"];
            if (hash.contains("algorithm") && hash["algorithm"].is_string() &&
                hash.contains("value") && hash["value"].is_string()) {
                file.expected_hash_algorithm = hash["algorithm"].get<std::string>();
                file.expected_hash = hash["value"].get<std::string>();
            }
        } else if (file_desc.contains("sha256") && file_desc["sha256"].is_string()) {
            file.expected_hash_algorithm = "sha256";
            file.expected_hash = file_desc["sha256"].get<std::string>();
        }

        plan.files.push_back(std::move(file));
    }

    return plan;
}

std::vector<FilesystemRequirement> plan_filesystem_requirements(const DownloadPlan& plan) {
    std::map<std::string, std::uint64_t> bytes_needed_by_target_path;
    for (const auto& file : plan.files) {
        const std::string target_dir =
            file.download_path.empty() ? plan.download_path : file.download_path;
        const std::string output_path = target_dir + "/" + file.name;
        const std::string partial_path = output_path + ".partial";
        const fs::path output_path_fs = path_from_utf8(output_path);
        const fs::path partial_path_fs = path_from_utf8(partial_path);

        std::uint64_t bytes_needed = file.size;
        if (safe_exists(output_path_fs) && !safe_exists(partial_path_fs)) {
            bytes_needed = 0;
        } else if (safe_exists(partial_path_fs)) {
            // Cap credit to the manifest size: a partial cannot save more than
            // the file costs, and a manifest can carry size=0 entries while
            // partials exist.
            std::error_code ec;
            std::uintmax_t partial_size = fs::file_size(partial_path_fs, ec);
            if (!ec) {
                const std::uint64_t credited =
                    (std::min)(static_cast<std::uint64_t>(partial_size), file.size);
                bytes_needed = file.size > credited ? file.size - credited : 0;
            }
        }

        if (bytes_needed > 0) {
            bytes_needed_by_target_path[target_dir] += bytes_needed;
        }
    }

    // Fold targets that share a filesystem so their combined remaining size is
    // checked once against it.
    using SpaceSignature = std::tuple<uintmax_t, uintmax_t, uintmax_t>;
    std::map<SpaceSignature, FilesystemRequirement> by_filesystem;
    for (const auto& [target_path, bytes_needed] : bytes_needed_by_target_path) {
        std::error_code ec;
        const auto space_info = fs::space(path_from_utf8(target_path), ec);
        if (ec) {
            continue;
        }

        auto& entry = by_filesystem[std::make_tuple(space_info.capacity, space_info.free,
                                                   space_info.available)];
        entry.bytes_needed += bytes_needed;
        if (entry.target_path.empty()) {
            entry.target_path = target_path;
        }
        entry.bytes_available = static_cast<std::uint64_t>(space_info.available);
    }

    std::vector<FilesystemRequirement> requirements;
    requirements.reserve(by_filesystem.size());
    for (auto& [signature, requirement] : by_filesystem) {
        requirements.push_back(std::move(requirement));
    }
    return requirements;
}

ArtifactTransfer::ArtifactTransfer(std::vector<std::unique_ptr<FileTransfer>> transfers)
    : transfers_(std::move(transfers)) {}

ArtifactTransfer ArtifactTransfer::with_default_transfers() {
    std::vector<std::unique_ptr<FileTransfer>> transfers;
    transfers.push_back(std::make_unique<HttpFileTransfer>());
    transfers.push_back(std::make_unique<LocalCopyFileTransfer>());
    return ArtifactTransfer(std::move(transfers));
}

void ArtifactTransfer::execute(const DownloadPlan& plan,
                               const std::map<std::string, std::string>& headers,
                               DownloadProgressCallback progress) const {
    const int total_files = static_cast<int>(plan.files.size());

    // Dispatch is settled before anything is written, so a file no transfer
    // handles fails without moving a byte.
    std::vector<const FileTransfer*> chosen;
    chosen.reserve(plan.files.size());
    for (const auto& file : plan.files) {
        const FileTransfer* match = nullptr;
        for (const auto& transfer : transfers_) {
            if (transfer->handles(file.url)) {
                match = transfer.get();
                break;
            }
        }
        if (!match) {
            const std::string scheme = file.url.substr(0, file.url.find(':'));
            std::ostringstream oss;
            oss << "No file transfer handles scheme '" << (scheme.empty() ? file.url : scheme)
                << "' for file '" << file.name << "'";
            throw std::runtime_error(oss.str());
        }
        chosen.push_back(match);
    }

    for (const auto& requirement : plan_filesystem_requirements(plan)) {
        if (requirement.bytes_needed <= requirement.bytes_available) {
            continue;
        }
        std::ostringstream oss;
        oss << "Insufficient disk space: download requires " << size_in_gb(requirement.bytes_needed)
            << " GB but only " << size_in_gb(requirement.bytes_available) << " GB is available on "
            << requirement.target_path;
        throw std::runtime_error(oss.str());
    }

    std::uint64_t total_download_size = 0;
    for (const auto& file : plan.files) {
        total_download_size += file.size;
    }

    int file_index = 0;
    for (const auto& file : plan.files) {
        file_index++;
        const std::string target_dir =
            file.download_path.empty() ? plan.download_path : file.download_path;
        const std::string output_path = target_dir + "/" + file.name;
        const std::string partial_path = output_path + ".partial";

        // Create parent directory for file (handles folders in filenames)
        ensure_create_directories(fs::path(output_path).parent_path());

        LOG(INFO, "ModelManager") << "Downloading: " << file.name << "..." << std::endl;

        if (progress) {
            DownloadProgress update;
            update.file = file.name;
            update.file_index = file_index;
            update.total_files = total_files;
            update.bytes_downloaded = 0;
            update.bytes_total = static_cast<size_t>(file.size);
            update.total_download_size = static_cast<size_t>(total_download_size);
            update.percent = 0;
            if (!progress(update)) {
                LOG(INFO, "ModelManager") << "Download cancelled by client" << std::endl;
                throw std::runtime_error("Download cancelled");
            }
        }

        // GGUF models are consumed directly by llama-server. If a previous
        // download saved an HTML or error page under a .gguf name, reject that
        // cache entry before it can be treated as already complete.
        if (gguf_reader_detail::ends_with_ignore_case(file.name, ".gguf") &&
            fs::exists(output_path) && !fs::exists(partial_path) &&
            !gguf_reader_detail::has_gguf_magic(output_path)) {
            LOG(WARNING, "ModelManager")
                << "Removing invalid GGUF cache file before download: " << file.name << std::endl;
            std::error_code remove_ec;
            fs::remove(path_from_utf8(output_path), remove_ec);
            if (remove_ec) {
                throw std::runtime_error("Invalid GGUF cache file could not be removed: " +
                                         output_path + " (" + remove_ec.message() + ")");
            }
        }

        std::uint64_t bytes_on_disk = 0;
        if (fs::exists(output_path) && !fs::exists(partial_path)) {
            bytes_on_disk = file.size;  // File already complete
        } else if (fs::exists(partial_path)) {
            bytes_on_disk = static_cast<std::uint64_t>(fs::file_size(partial_path));
        }

        ByteProgress byte_progress;
        if (progress) {
            byte_progress = [&, file_index, bytes_on_disk](std::size_t downloaded,
                                                           std::size_t total) -> bool {
                DownloadProgress update;
                update.file = file.name;
                update.file_index = file_index;
                update.total_files = total_files;
                update.bytes_downloaded = downloaded;
                update.bytes_total = total;
                update.total_download_size = static_cast<size_t>(total_download_size);
                update.bytes_previously_downloaded = static_cast<size_t>(bytes_on_disk);
                update.percent = (total > 0) ? static_cast<int>((downloaded * 100) / total) : 0;
                return progress(update);
            };
        }

        std::uint64_t bytes_moved = 0;
        try {
            bytes_moved =
                chosen[file_index - 1]->transfer(file, target_dir, headers, byte_progress);
        } catch (const TransferError& error) {
            if (error.cancelled) {
                LOG(INFO, "ModelManager") << "Download cancelled by client" << std::endl;
                throw std::runtime_error("Download cancelled");
            }

            std::ostringstream error_msg;
            error_msg << "Failed to download file: " << file.name << "\n";
            error_msg << "URL: " << file.url << "\n";
            error_msg << error.what();
            if (fs::exists(output_path)) {
                const std::uintmax_t partial_size = fs::file_size(output_path);
                if (partial_size > 0) {
                    error_msg << "\n\n[INFO] Partial download preserved at: " << output_path;
                    error_msg << "\n[INFO] Partial size: " << std::fixed << std::setprecision(1)
                              << (partial_size / (1024.0 * 1024.0)) << " MB";
                    error_msg
                        << "\n[INFO] Run the command again to resume from where it left off.";
                }
            }
            throw std::runtime_error(error_msg.str());
        }

        LOG(INFO, "ModelManager") << "Downloaded: " << file.name << std::endl;

        if (bytes_moved == 0) {
            // Report a skipped file as complete so a resumed download still
            // reaches 100%.
            if (progress) {
                DownloadProgress update;
                update.file = file.name;
                update.file_index = file_index;
                update.total_files = total_files;
                update.bytes_downloaded = static_cast<size_t>(file.size);
                update.bytes_total = static_cast<size_t>(file.size);
                update.total_download_size = static_cast<size_t>(total_download_size);
                update.bytes_previously_downloaded = static_cast<size_t>(file.size);
                update.percent = 100;
                (void)progress(update);
            }
        }
    }

    LOG(INFO, "ModelManager") << "Validating downloaded files..." << std::endl;
    bool all_valid = true;
    for (const auto& file : plan.files) {
        const std::string target_dir =
            file.download_path.empty() ? plan.download_path : file.download_path;
        const std::string expected_path = target_dir + "/" + file.name;
        const std::string partial_path = expected_path + ".partial";

        if (fs::exists(partial_path)) {
            if (fs::exists(expected_path)) {
                LOG(INFO, "ModelManager")
                    << "Removing stale partial file: " << file.name << ".partial" << std::endl;
                std::error_code ec;
                fs::remove(partial_path, ec);
            } else {
                all_valid = false;
                LOG(ERROR, "ModelManager") << "Incomplete file found: " << file.name << ".partial"
                                           << std::endl;
                continue;
            }
        }

        if (!fs::exists(expected_path)) {
            all_valid = false;
            LOG(ERROR, "ModelManager") << "Missing file: " << file.name << std::endl;
            continue;
        }

        // Tree API sizes can differ from actual LFS object sizes in edge cases,
        // so a mismatch is noted rather than fatal.
        if (file.size > 0) {
            const std::uintmax_t actual_size = fs::file_size(expected_path);
            if (actual_size != file.size) {
                LOG(WARNING, "ModelManager")
                    << "Size note for " << file.name << ": tree API reports " << file.size
                    << " bytes, actual " << actual_size << " bytes" << std::endl;
            }
        }

        // llama.cpp fails late and opaquely when a cached GGUF path points to an
        // HTML or error page, so surface that here and drop the bad entry.
        if (gguf_reader_detail::ends_with_ignore_case(file.name, ".gguf") &&
            !gguf_reader_detail::has_gguf_magic(expected_path)) {
            all_valid = false;
            LOG(ERROR, "ModelManager") << "Invalid GGUF file: " << file.name
                                       << " (missing GGUF magic header)" << std::endl;
            std::error_code remove_ec;
            fs::remove(path_from_utf8(expected_path), remove_ec);
            if (remove_ec) {
                LOG(ERROR, "ModelManager") << "Failed to remove invalid GGUF file: "
                                           << remove_ec.message() << std::endl;
            }
            continue;
        }
    }

    if (!all_valid) {
        throw std::runtime_error(
            "Download validation failed. Some files are incomplete or missing. "
            "Run the command again to resume.");
    }
}

bool HttpFileTransfer::handles(const std::string& url) const {
    const std::string lower = lower_ascii(url);
    return lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0;
}

std::uint64_t HttpFileTransfer::transfer(const DownloadFile& file,
                                         const std::string& target_dir,
                                         const std::map<std::string, std::string>& headers,
                                         const ByteProgress& progress) const {
    utils::DownloadOptions options;
    options.max_retries = 10;
    options.initial_retry_delay_ms = 2000;
    options.max_retry_delay_ms = 120000;
    options.resume_partial = true;
    options.low_speed_limit = 1000;
    options.low_speed_time = static_cast<int>(utils::HttpClient::get_default_timeout());
    options.connect_timeout = 60;
    options.expected_hash_algorithm = file.expected_hash_algorithm;
    options.expected_hash = file.expected_hash;

    utils::ProgressCallback http_progress = nullptr;
    if (progress) {
        http_progress = [&progress](std::size_t downloaded, std::size_t total) -> bool {
            return progress(downloaded, total);
        };
    } else {
        http_progress = utils::create_throttled_progress_callback();
    }

    const auto result = HttpClient::download_file(file.url, target_dir + "/" + file.name,
                                                 http_progress, headers, options);
    if (result.cancelled) {
        throw TransferError("Download cancelled", true);
    }
    if (!result.success) {
        throw TransferError(result.error_message);
    }
    return static_cast<std::uint64_t>(result.bytes_downloaded);
}

bool LocalCopyFileTransfer::handles(const std::string& url) const {
    return lower_ascii(url).rfind(kFileScheme, 0) == 0;
}

std::uint64_t LocalCopyFileTransfer::transfer(const DownloadFile& file,
                                              const std::string& target_dir,
                                              const std::map<std::string, std::string>&,
                                              const ByteProgress& progress) const {
    const std::string source_path = path_from_file_url(file.url);
    const std::string output_path = target_dir + "/" + file.name;
    const std::string partial_path = output_path + ".partial";

    std::error_code ec;
    if (!fs::exists(path_from_utf8(source_path), ec)) {
        throw TransferError("Local source file not found: " + source_path);
    }
    const std::uintmax_t source_size = fs::file_size(path_from_utf8(source_path), ec);
    if (ec) {
        throw TransferError("Could not read local source file: " + source_path + " (" +
                            ec.message() + ")");
    }

    if (fs::exists(path_from_utf8(output_path)) && !fs::exists(path_from_utf8(partial_path))) {
        return 0;
    }

    std::ifstream source(path_from_utf8(source_path), std::ios::binary | std::ios::in);
    if (!source) {
        throw TransferError("Could not open local source file: " + source_path);
    }

    std::ofstream staged(path_from_utf8(partial_path), std::ios::binary | std::ios::trunc);
    if (!staged) {
        throw TransferError("Could not create staged file: " + partial_path);
    }

    constexpr std::size_t chunk_size = 1 << 20;
    std::vector<char> buffer(chunk_size);
    std::uint64_t copied = 0;
    while (source) {
        source.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize read = source.gcount();
        if (read <= 0) {
            break;
        }
        staged.write(buffer.data(), read);
        if (!staged) {
            staged.close();
            throw TransferError("Could not write staged file: " + partial_path);
        }
        copied += static_cast<std::uint64_t>(read);
        if (progress && !progress(static_cast<std::size_t>(copied),
                                  static_cast<std::size_t>(source_size))) {
            staged.close();
            throw TransferError("Download cancelled", true);
        }
        if (read < static_cast<std::streamsize>(buffer.size())) {
            break;
        }
    }
    staged.close();
    source.close();

    if (!file.expected_hash.empty()) {
        std::string hash_error;
        if (!HttpClient::verify_file_digest(partial_path, file.expected_hash_algorithm,
                                            file.expected_hash, hash_error)) {
            std::error_code remove_ec;
            fs::remove(path_from_utf8(partial_path), remove_ec);
            throw TransferError("Content verification failed for " + file.name + ": " +
                                hash_error);
        }
    }

    fs::rename(path_from_utf8(partial_path), path_from_utf8(output_path), ec);
    if (ec) {
        throw TransferError("Could not move staged file into place: " + output_path + " (" +
                            ec.message() + ")");
    }
    return copied;
}

}  // namespace models
}  // namespace lemon

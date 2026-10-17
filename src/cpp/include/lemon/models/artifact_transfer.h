#pragma once

#include <lemon/models/download_progress.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace lemon {
namespace models {

struct DownloadFile {
    std::string name;
    std::string url;
    std::uint64_t size = 0;
    std::string expected_hash_algorithm;
    std::string expected_hash;
    std::string download_path;  // Empty uses the plan's target directory.
};

struct DownloadPlan {
    std::string download_path;
    std::vector<DownloadFile> files;
};

// Reads the manifest written by the registry download path.
DownloadPlan download_plan_from_manifest(const nlohmann::json& manifest);

// `cancelled` distinguishes a client cancel from a transfer failure.
class TransferError : public std::runtime_error {
public:
    explicit TransferError(const std::string& message, bool cancelled = false)
        : std::runtime_error(message), cancelled(cancelled) {}

    bool cancelled;
};

// Returns false to cancel.
using ByteProgress = std::function<bool(std::size_t downloaded, std::size_t total)>;

// One scheme per implementation; space, progress and cancel live in ArtifactTransfer.
class FileTransfer {
public:
    virtual ~FileTransfer() = default;

    virtual bool handles(const std::string& url) const = 0;

    // Returns the bytes moved, zero when the target was already complete.
    virtual std::uint64_t transfer(const DownloadFile& file,
                                   const std::string& target_dir,
                                   const std::map<std::string, std::string>& headers,
                                   const ByteProgress& progress) const = 0;
};

class HttpFileTransfer : public FileTransfer {
public:
    bool handles(const std::string& url) const override;
    std::uint64_t transfer(const DownloadFile& file,
                           const std::string& target_dir,
                           const std::map<std::string, std::string>& headers,
                           const ByteProgress& progress) const override;
};

// No retries: a short read is a disk error. The source is never modified.
class LocalCopyFileTransfer : public FileTransfer {
public:
    bool handles(const std::string& url) const override;
    std::uint64_t transfer(const DownloadFile& file,
                           const std::string& target_dir,
                           const std::map<std::string, std::string>& headers,
                           const ByteProgress& progress) const override;
};

// One filesystem that a plan writes to, and what it still needs.
struct FilesystemRequirement {
    std::string target_path;
    std::uint64_t bytes_needed = 0;
    std::uint64_t bytes_available = 0;
};

// Still to write, crediting bytes on disk; same-filesystem targets fold.
std::vector<FilesystemRequirement> plan_filesystem_requirements(const DownloadPlan& plan);

// The one place a plan is executed: space, dispatch, progress, cancel, verify.
class ArtifactTransfer {
public:
    explicit ArtifactTransfer(std::vector<std::unique_ptr<FileTransfer>> transfers);

    static ArtifactTransfer with_default_transfers();

// Per call, never on the plan, so credentials cannot be logged with it.
    void execute(const DownloadPlan& plan,
                 const std::map<std::string, std::string>& headers,
                 DownloadProgressCallback progress) const;

private:
    std::vector<std::unique_ptr<FileTransfer>> transfers_;
};

}  // namespace models
}  // namespace lemon

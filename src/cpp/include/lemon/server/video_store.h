#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace lemon {
namespace video_store {

// Clips and input images live here rather than in the job record: the job store
// is rewritten on every state change of every job.
std::filesystem::path dir();

std::string new_file_name();

// Only names made by new_file_name() are accepted, so a request can never point a
// read or a write outside dir().
bool is_valid_file_name(const std::string& name);

void write(const std::string& name, const std::string& bytes);

bool read(const std::string& name, std::string& bytes);

bool exists(const std::string& name);

// Reserves `preferred` for the job `job_id` when it is a valid name that no file or
// other reservation uses, otherwise a new name, and returns the reserved one. A
// reservation keeps the clip safe from clean_up() from before it is written until the
// job's record names it, which covers the gap between writing a clip and the job
// recording it, or until the job is gone.
std::string reserve_file_name(const std::string& preferred, const std::string& job_id);

// Drops a reservation whose clip was never written, such as after a failed generation.
void release(const std::string& name);

// Keeps input files reserved until the job that names them has been created.
// On scope exit, unreferenced files become eligible for the next sweep.
class StagedImages {
public:
    StagedImages() = default;
    ~StagedImages();
    StagedImages(const StagedImages&) = delete;
    StagedImages& operator=(const StagedImages&) = delete;
    std::string stage(const std::string& base64);

private:
    std::vector<std::string> names_;
};

// Rehydrates a server-owned image_file into the backend's raw base64 image field.
// Missing, invalid, oversized or conflicting references return an error.
std::string restore_image(nlohmann::json& request);

struct CleanupSnapshot {
    std::set<std::string> referenced;  // clip and input image names in job records
    std::set<std::string> live_jobs;   // ids of the jobs that still exist
};

// Takes the snapshot and sweeps under one lock, so no other clean-up or reservation
// can change in between: removes every stored file that is neither referenced nor
// reserved, and drops reservations for published files or deleted jobs.
void clean_up(const std::function<CleanupSnapshot()>& snapshot);

// Upper bounds for a video request's numbers, shared by the route's documented schema
// and normalize_request().
inline constexpr int kMaxVideoFrames = 1024;
inline constexpr int kMaxFps = 120;
inline constexpr int kMaxDimension = 4096;
inline constexpr int kMaxSteps = 1000;
inline constexpr double kMaxDuration = 600.0;
// Bound the encoded image input size, both inline and when restored from disk.
inline constexpr std::size_t kMaxImageBase64Bytes = 8 * 1024 * 1024;

// Checks the request's numbers and optional start image for both the /videos route and a generic
// generate_video job. An integer-valued float such as 9.0 is stored as the integer 9,
// so the backend never mistakes it for a missing value. Returns an error message, or an
// empty string when the request is usable.
std::string normalize_request(nlohmann::json& request);

// Video ids are job ids, so without this check the video routes would report on, or
// cancel, any job.
bool is_video_job(const nlohmann::json& job);

// An interrupted job is a canceled video, unless the job engine recorded why it stopped:
// it only does so when the server restarted mid-generation, and that video failed.
// A pause only takes effect between steps, so a video paused while its one step ran
// is held paused with the clip already made: that video is completed.
std::string video_status(const nlohmann::json& job);

} // namespace video_store
} // namespace lemon

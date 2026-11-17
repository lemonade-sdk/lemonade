#pragma once

#include <filesystem>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

namespace lemon {
namespace video_store {

// Encoded clips live here rather than in the job record: the job store is rewritten on
// every state change of every job, so a few clips in it made each transition copy
// hundreds of megabytes.
std::filesystem::path dir();

std::string new_file_name();

// Only names made by new_file_name() are accepted, so a request can never point a
// read or a write outside dir().
bool is_valid_file_name(const std::string& name);

void write(const std::string& name, const std::string& bytes);

bool read(const std::string& name, std::string& bytes);

bool exists(const std::string& name);

// Reserves `preferred` when it is a valid name that no file or other reservation uses,
// otherwise a new name, and returns the reserved one. A reservation keeps the clip safe
// from remove_all_except() from before it is written until a clean-up sees its name in
// a job record, which closes the gap between writing a clip and the job recording it.
std::string reserve_file_name(const std::string& preferred);

// Drops a reservation whose clip was never written, such as after a failed generation.
void release(const std::string& name);

// Removes every clip that is neither in `keep` nor reserved, and drops the reservations
// that `keep` now covers.
void remove_all_except(const std::set<std::string>& keep);

// Upper bounds for a video request's numbers, shared by the route's documented schema
// and normalize_request().
inline constexpr int kMaxVideoFrames = 1024;
inline constexpr int kMaxFps = 120;
inline constexpr int kMaxDimension = 4096;
inline constexpr int kMaxSteps = 1000;
inline constexpr double kMaxDuration = 600.0;

// Checks the request's numbers once, for both the /videos route and a generic
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

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

void remove_all_except(const std::set<std::string>& keep);

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

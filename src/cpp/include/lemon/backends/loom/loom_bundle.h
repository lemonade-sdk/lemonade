#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace lemon {
namespace backends {
namespace loom {

using json = nlohmann::json;

inline constexpr const char* kCardFile = "model.json";
inline constexpr const char* kLockFile = "model.json.lock";
inline constexpr const char* kInfoFile = "info.json";
inline constexpr const char* kCompleteMarker = ".lemonade-loom.json";
// Bound on the card, lock, and info.json read while inspecting a repository.
inline constexpr std::uint64_t kMaxMetadataBytes = 8 * 1024 * 1024;
// Loom release the bundles are fetched for: the loom-fun tag of the cards this
// loom-serve reads, and the backend_versions.json pin. A snapshot Lemonade recorded
// for another release (an older card loom-serve rejects) is fetched again.
inline constexpr const char* kBundleRevision = "v0.2.0";

// Keys of info.json's `devices` map, one per GPU a bundle is built and gated for.
inline constexpr const char* kStrixHaloTarget = "strix-halo";
inline constexpr const char* kRadeonR9700Target = "radeon-r9700";
// gfx1201 is configured by VRAM class: radeon-r9700 is the 32 GB class (R9700),
// gfx1201-16gb the 16 GB class (RX 9070 XT, RX 9070).
inline constexpr const char* kGfx120116gbTarget = "gfx1201-16gb";

// A parameter the card places beside itself but takes from a pinned revision
// of another repository.
struct OriginFile {
    std::string path;        // relative to the card, e.g. parameter/x.gguf
    std::string repository;  // e.g. unsloth/Qwen3.5-0.8B-GGUF
    std::string revision;    // commit the card pins
    std::string file;        // path inside that repository
    std::uint64_t size = 0;
    std::string sha256;
};

// A file the bundle repository itself ships whose digest the lock records.
struct LockedFile {
    std::string path;
    std::uint64_t size = 0;
    std::string sha256;
};

struct BundlePlan {
    std::string model_id;
    std::vector<OriginFile> origin_files;
    std::vector<LockedFile> locked_files;
};

// What the card and its lock require beyond the bundle repository's own files.
// Throws std::runtime_error when either document is malformed, names an origin
// kind other than "hf", or leaves a file without a locked digest.
BundlePlan plan_bundle(const json& card, const json& lock);

// The program sources and data files the card's program sets read. A lock
// published with the default "assets" scope leaves them unrecorded.
// Throws std::runtime_error when one escapes the card's folder.
std::vector<std::string> program_files(const json& card);

// A relative path that stays inside the folder it is joined to.
bool is_contained_relative_path(const std::string& path);

// Throws std::invalid_argument unless `repo_id` is a plain `org/name` repository id.
void validate_repo_id(const std::string& repo_id);

// The device target a GPU runs, or "" when Loom has no build for it. `arch` is
// what identify_rocm_arch_from_name gives for `gpu_name`: the exact ISA on Linux,
// but only the gfx120X family on Windows, where the name tells gfx1201 (Navi 48:
// R9700, RX 9070) from gfx1200 (RX 9060). gfx1201 is chosen by VRAM class
// (`vram_gb`, 0 when unknown): >= 30 GiB is the 32 GB class (radeon-r9700),
// 14 to 30 GiB the 16 GB class (gfx1201-16gb), less has no build. An unknown
// size is taken as the 32 GB class.
std::string device_target(const std::string& arch, const std::string& gpu_name,
                          double vram_gb = 0.0);

// False when info.json gates its targets and `target` is missing or marked
// unsupported (a bundle too large for the R9700's 32 GB) or not a target. A
// bundle without that map is left to loom-serve.
bool bundle_supports_target(const json& info, const std::string& target);

struct PresetLimits {
    std::int64_t context_capacity = 0;
    std::int64_t max_tokens = 0;
};

PresetLimits preset_limits(const json& card, const std::string& preset = "");

struct QuickStart {
    std::string preset;
    std::string device_query;
    std::vector<std::string> args;
};

// Throws when `info` records no Quick start for `target`, or one Lemonade does not allow.
QuickStart quick_start(const json& info, const std::string& target);

// Whether `info` records a Quick start for `target` at all (a bundle fetched
// before cards recorded one does not).
bool records_quick_start(const json& info, const std::string& target);

// --parameter_mode=preload: reads the weights into host memory first, which
// avoids ROCm/hrx-system#3332 (the default parameter mode fails under the stock
// 8 MiB RLIMIT_MEMLOCK on Linux).
inline constexpr const char* kPreloadFlag = "--parameter_mode=preload";

// loom-serve rejects a memory limit on a fixed pool, so a custom --memory_bytes
// drops the recorded backing and size before custom arguments are appended.
std::vector<std::string> serve_args(const QuickStart& quick_start,
                                    bool preload, const std::string& custom_args);

std::vector<std::string> build_server_argv(const std::string& model_dir, int port,
                                           const std::string& preset,
                                           const std::vector<std::string>& serve_args,
                                           const std::string& custom_args,
                                           const std::string& device_query = "");

// loom-serve registers the bundle under the card's model id and reads
// chat_template_kwargs itself, so the `/no_think` prefix Lemonade adds for other
// templates would reach the model as user text.
json prepare_chat_request(json request, const std::string& model_id);
json normalize_chat_response(json response, const json& request, const std::string& fallback_model);

enum class Readiness { Pending, Ready, Failed };

struct ReadinessState {
    Readiness state = Readiness::Pending;
    std::string error;
};

// /readyz answers as soon as the listener is up; the bundle is usable once
// /v1/models lists the card's id, and lost once /v1/status reports it failed.
ReadinessState parse_readiness(const json& models, const json& status,
                               const std::string& model_id);

// loom-serve reports device and card errors on its console before it listens,
// so a failed start is explained by the last lines it printed.
std::string startup_failure_detail(const std::vector<std::string>& output_lines);

// So a bundle found by search registers like the curated rows Lemonade ships.
std::vector<std::string> suggested_labels(const json& info, const std::string& name);
std::string suggested_name(const std::string& repo_id, const json& card);
std::uint64_t locked_bytes(const json& lock);

}  // namespace loom
}  // namespace backends
}  // namespace lemon

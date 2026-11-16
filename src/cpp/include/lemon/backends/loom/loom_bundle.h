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
// Hugging Face tag every loom-fun bundle repository is cloned at; matches the Loom release.
inline constexpr const char* kBundleRevision = "v0.1.0";

// Keys of info.json's `devices` map, one per GPU a bundle is built and gated for.
inline constexpr const char* kStrixHaloTarget = "strix-halo";
inline constexpr const char* kRadeonR9700Target = "radeon-r9700";

// A parameter the card places in the run folder but takes from a pinned
// revision of another repository.
struct OriginFile {
    std::string path;        // relative to the run folder, e.g. parameter/x.gguf
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
    // "chat" when the card has it, else the card's only preset; "" leaves the
    // choice to loom-serve.
    std::string preset;
    std::vector<OriginFile> origin_files;
    std::vector<LockedFile> locked_files;
};

// What the card and its lock require beyond the bundle repository's own files.
// Throws std::runtime_error when either document is malformed, names an origin
// kind other than "hf", or leaves a file without a locked digest.
BundlePlan plan_bundle(const json& card, const json& lock);

// A relative path that stays inside the folder it is joined to.
bool is_contained_relative_path(const std::string& path);

// `<org>--<name>` for an `org/name` bundle repository id.
std::string store_dir_name(const std::string& repo_id);

// The device target a GPU runs, or "" when Loom has no build for it. `arch` is
// what identify_rocm_arch_from_name gives for `gpu_name`: the exact ISA on Linux,
// but only the gfx120X family on Windows, where the name tells gfx1201 (Navi 48:
// R9700, RX 9070) from gfx1200 (RX 9060).
std::string device_target(const std::string& arch, const std::string& gpu_name);

// False when info.json gates its targets and `target` is missing or marked
// unsupported (a bundle too large for the R9700's 32 GB). A bundle without that
// map is left to loom-serve.
bool bundle_supports_target(const json& info, const std::string& target);

std::vector<std::string> build_server_argv(const std::string& model_dir, int port,
                                           const std::string& preset,
                                           const std::string& device,
                                           const std::string& memory_bytes,
                                           const std::string& pool_backing,
                                           const std::string& custom_args);

// loom-serve registers the bundle under the card's model id and reads
// chat_template_kwargs itself, so the `/no_think` prefix Lemonade adds for other
// templates would reach the model as user text.
json prepare_chat_request(json request, const std::string& model_id);

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

// The pool size a card asks for, in the byte-string form --memory_bytes takes,
// or "" when the card does not say.
std::string card_memory_bytes(const json& info);

struct PoolSettings {
    std::string memory_bytes;
    std::string pool_backing;
};

// The card's memory figure is sized for Strix Halo's unified memory, so the
// R9700 backs the pool with its fixed VRAM instead. loom-serve rejects a
// memory limit on a fixed pool, so a size the user asks for keeps it elastic.
PoolSettings pool_settings(const std::string& target, const std::string& requested_memory_bytes,
                           const json& info);

// So a bundle found by search registers like the curated rows Lemonade ships.
std::vector<std::string> suggested_labels(const json& info, const std::string& name);
std::string suggested_name(const std::string& repo_id, const json& card);
std::uint64_t locked_bytes(const json& lock);

}  // namespace loom
}  // namespace backends
}  // namespace lemon

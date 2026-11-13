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
    // The card's only preset when it has no "chat" one; "" leaves the choice to
    // loom-serve.
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

std::vector<std::string> build_server_argv(const std::string& model_dir, int port,
                                           const std::string& preset,
                                           const std::string& device,
                                           const std::string& memory_bytes,
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

// The pool size a card asks for, in the byte-string form --memory_bytes takes,
// or "" when the card does not say.
std::string card_memory_bytes(const json& info);

// The rules docs/tools/gen_loom_models.py applies to the curated catalog, so a
// bundle found by search registers like the rows Lemonade ships.
std::vector<std::string> suggested_labels(const json& info, const std::string& name);
std::string suggested_name(const std::string& repo_id, const json& card);
std::uint64_t locked_bytes(const json& lock);

}  // namespace loom
}  // namespace backends
}  // namespace lemon

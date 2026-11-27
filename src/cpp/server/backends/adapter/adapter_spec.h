#pragma once

// Engine adapter specs: the pure part of the adapter backend. Everything here reads JSON and
// returns values or throws SpecError; nothing starts a process, opens a socket or reads a file,
// so each function is tested on its own (test/cpp/adapter_spec/). adapter_server.cpp does the I/O.

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace lemon {
namespace backends {
namespace adapter {
namespace specs {

using json = nlohmann::json;

// The newest spec schema this backend reads.
inline constexpr int kSpecVersion = 1;

// Every failure here names what is wrong; the message is shown to the user as is.
struct SpecError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---- helpers that already existed in adapter_server.cpp -------------------------------------

// A spec name is a file stem in the adapters folder: letters, digits, - _ . ; at most 64; no "..".
bool valid_spec_name(const std::string& name);
// Replaces every occurrence of each key (e.g. "{port}") in text with its value.
std::string fill(std::string text, const std::map<std::string, std::string>& subs);
// Splits on whitespace (no quoting).
std::vector<std::string> split_args(const std::string& text);
// True when `values` is a JSON array holding the string v.
bool contains(const json& values, const std::string& v);

// ---- the argument vector (R1, R16) ----

// The placeholders a spec may use. Their values come from lemond, never from the spec.
inline const std::vector<std::string> kPlaceholders = {
    "{port}", "{model}", "{model_path}", "{device}", "{gpu_uuids}", "{ctx}", "{spec_dir}"};

// Builds the engine's argv (without the program) from spec["args"], filling each placeholder from
// `values` (keys like "{port}"). A placeholder is "{", an identifier ([A-Za-z_][A-Za-z0-9_]*), "}"; any
// other braces (e.g. "{}") are plain text. Each element stays one argv entry after filling, even when a
// value contains spaces. Throws SpecError, naming the field or token, when "args" is not an array of
// strings, an element uses a placeholder outside kPlaceholders or one `values` does not supply, or the
// spec_version is not an integer. "args_template" (one string, split on whitespace) is read only when
// "args" is absent and spec_version is 1 or absent. No "args" and no template gives an empty argv.
std::vector<std::string> build_argv(const json& spec, const std::map<std::string, std::string>& values);

// ---- the environment (R3) ----

// Variable names a spec may set: any name starting with one of these prefixes ...
inline const std::vector<std::string> kEnvPrefixes = {
    "CUDA_", "HIP_", "ROCR_", "HSA_", "GGML_", "LLAMA_", "OMP_", "MKL_", "VK_", "NAUTIBURN_", "STRATA_", "LEMONADE_ENGINE_"};
// ... and these exact names.
inline const std::vector<std::string> kEnvNames = {"LD_LIBRARY_PATH", "TZ"};

// Builds the engine's extra environment from a spec's "env" object (name -> string), filling
// placeholders from `values` as build_argv does. Returns pairs in the object's key order.
// Throws SpecError when: env is not an object (null or absent is fine: empty result); a value is
// not a string; a name is not allowed (named in the message); LD_LIBRARY_PATH has an entry (split
// on ':', empty entries ignored) that is not absolute or not inside one of `allowed_dirs` after
// filling (a dir D contains P when P == D or P starts with D + "/"; an entry containing ".." is
// refused). The refused entry is named in the message. The returned value is the filled string as given.
std::vector<std::pair<std::string, std::string>> build_env(
    const json& env, const std::map<std::string, std::string>& values, const std::vector<std::string>& allowed_dirs);

// LD_LIBRARY_PATH as handed to the engine: empty entries removed (including leading, trailing and
// repeated ':'). An empty entry makes the dynamic loader search the current working directory, which
// a spec does not control. Other values are returned as given. Applied where the launch environment
// is assembled, so build_env keeps returning the filled string as given.
std::string launch_env_value(const std::string& name, const std::string& value);

// ---- which card (R11) ----

struct Gpu {
    int index;         // the driver's index on this host
    std::string uuid;  // NVIDIA "GPU-..."; AMD "amd:<device>:<subsystem>" (the fleet's id scheme)
};
struct DeviceChoice {
    int index;
    std::string uuid;
};

// Resolves the device to run on. `requested` (the user's adapter_device option) wins when not
// empty, else `spec_default`, else "0". The text is a decimal index (e.g. "1") or a uuid that must
// match one entry of `inventory` exactly (case-insensitive). An index is matched against
// Gpu::index, never the position in `inventory` (an inventory may be sparse, e.g. indexes 1 and 3,
// or unordered). Throws SpecError when no entry has that index, the uuid matches none, the number
// does not fit an int, or the text is neither. Leading/trailing spaces ignored.
DeviceChoice choose_device(const std::string& requested, const std::string& spec_default, const std::vector<Gpu>& inventory);

// The values of the {device} and {gpu_uuids} placeholders.
struct DeviceValues {
    std::string device;     // the card's index as decimal text
    std::string gpu_uuids;  // the card's uuid, or the index text when the card has no uuid
};
// With an empty inventory (lemond found no card list) the text is used as given, trimmed, and
// gpu_uuids equals device. Otherwise choose_device decides, its SpecError propagates, and gpu_uuids is
// the card's uuid, or the index text when the card has none.
DeviceValues device_values(const std::string& requested, const std::string& spec_default, const std::vector<Gpu>& inventory);

// ---- is this a spec we can run (R16) ----

struct SpecCheck {
    int level_cap;                      // 0: run without control (provider fallback); 4: no cap
    std::vector<std::string> problems;  // one line each, empty when the spec is fine
};

// Checks a parsed spec without running it. Required: "binary" (non-empty string) and either
// "args" (array) or "args_template" (string). Optional fields, when present, must have their type:
// "spec_version" integer >= 1, "env" object, "ready" object, "telemetry" object, "telemetry_path"
// string, "memory_path" string, "budget_path" string, "binary_sha256" 64 hex chars,
// "default_device_index" string. Unknown fields are ignored (no problem reported).
// A spec_version above kSpecVersion, a missing required field, or a wrong-typed field adds one
// problem each and sets level_cap to 0; otherwise level_cap is 4. A spec that is not a JSON
// object gives level_cap 0 with one problem.
SpecCheck check_spec(const json& spec);

// ---- memory by device and tier (R9) ----

struct DeviceMemory {
    std::string uuid;
    uint64_t bytes;
    bool evictable;  // the engine can give these bytes back without unloading (cache)
};
struct MemoryReport {
    std::vector<DeviceMemory> devices;
    uint64_t pinned_host = 0;  // page-locked host RAM
    uint64_t remote = 0;       // memory held on other hosts (e.g. an RDMA expert window)
    uint64_t disk = 0;         // tiers on local disk
};

// Parses an engine's memory answer: {"devices":[{"uuid":"GPU-..","bytes":N,"evictable":bool}],
// "pinned_host":N, "remote":N, "disk":N}. Missing tiers are 0, "evictable" defaults to false and
// "devices" may be absent. Throws SpecError for a wrong type, a negative count or a bad uuid.
MemoryReport parse_memory_report(const json& body);
// The one number lemond's evictor sees: device bytes plus pinned_host (remote and disk excluded).
uint64_t local_bytes(const MemoryReport& report);
// Bytes on the device with this uuid (case-insensitive), 0 when absent.
uint64_t device_bytes(const MemoryReport& report, const std::string& uuid);

// The "adapter_memory" stats object: the parsed report plus "local_bytes", or {"error": "<message>"}
// when the answer does not parse. Never throws.
json memory_telemetry(const json& body);

// ---- shrink instead of unload (R10) ----

// The body POSTed to the engine's budget_path: {"device":"<uuid>","mib":N}. Throws SpecError when
// uuid is empty or mib is negative.
json budget_request(const std::string& uuid, int64_t mib);

struct BudgetResult {
    bool accepted;
    uint64_t now_bytes;   // the engine's bytes on that device after the change
    std::string reason;   // filled when not accepted
};
// Parses {"accepted":bool,"now_bytes":N,"reason":"..."} ("reason" optional). Throws SpecError when
// "accepted" is missing or not a bool, or an accepted reply has no valid now_bytes.
BudgetResult parse_budget_reply(const json& body);

// ---- ready states (R5) ----

enum class ReadyState { Starting, Loading, Ready, Warm, Error };
struct Readiness {
    ReadyState state;
    double progress;  // 0..1 while Loading when the engine says, else -1
};

// Maps an engine's status answer to a state using the spec's "ready" object:
//   {"json_field": "status", "ready_values": [...], "warm_values": [...], "loading_values": [...],
//    "error_values": [...], "progress_field": "progress"}
// No json_field: any object answer is Ready. With json_field: a missing field or a value in none of
// the lists is Starting; error_values -> Error; warm_values -> Warm; ready_values -> Ready;
// loading_values -> Loading. A non-object status is Starting. progress is read from
// progress_field only in Loading: a number in 0..1 is used as is, a number in (1..100] is divided by
// 100, anything else gives -1.
Readiness ready_state(const json& status, const json& ready_spec);

}  // namespace specs
}  // namespace adapter
}  // namespace backends
}  // namespace lemon

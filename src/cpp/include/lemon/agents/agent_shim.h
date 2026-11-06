#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "lemon/agents/agent_paths.h"

namespace lemon::agents {

// The line that marks a file Lemonade owns. A file without it is never
// modified or deleted.
constexpr const char* kShimMarker = "lemonade-agent-shim v1: managed by lemonade, do not edit";

enum class ShimState { Ours, Foreign, Absent };

std::filesystem::path shim_path(const AgentPaths& paths, const std::string& binary_name);

std::string render_posix_shim(const std::string& agent_id, const std::filesystem::path& target);
std::string render_cmd_shim(const std::string& agent_id, const std::filesystem::path& target);

bool is_lemonade_shim(const std::filesystem::path& file);
ShimState shim_state(const std::filesystem::path& file);
const char* shim_state_name(ShimState state);

// Writes the shim unless the file is foreign; leaves an identical shim alone.
// Returns false when a foreign file is in the way. Throws on write failures.
bool write_shim(const std::filesystem::path& file, const std::string& content);

// Removes the shim if it is ours. Returns whether a file was removed.
bool remove_shim(const std::filesystem::path& file);

// Every executable named `name` on path_value (PATH syntax for this OS), in
// order, as absolute paths. On Windows each PATHEXT extension is tried.
std::vector<std::filesystem::path> path_matches(const std::string& name,
                                                const std::string& path_value,
                                                const std::string& pathext = "");

} // namespace lemon::agents

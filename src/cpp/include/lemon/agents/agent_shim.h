#pragma once

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "lemon/agents/agent_paths.h"

namespace lemon::agents {

// The line that marks a file Lemonade owns. A file without it is never
// modified or deleted.
constexpr const char* kShimMarker = "lemonade-agent-shim v1: managed by lemonade, do not edit";

enum class ShimState { Ours, Foreign, Absent };

std::filesystem::path shim_path(const AgentPaths& paths, const std::string& binary_name);

std::string render_posix_shim(const std::string& agent_id, const std::filesystem::path& target);
// cmd.exe reads batch files in the console code page, so a target below one
// of env_dirs (name, UTF-8 value) is written through %name% instead.
std::string render_cmd_shim(
    const std::string& agent_id, const std::filesystem::path& target,
    const std::vector<std::pair<std::string, std::string>>& env_dirs = {});

// The launchd job `lemonade agents enable` writes on macOS. Like the systemd
// unit, it reads ~/.config/lemonade/lemond.conf and agent-host.env. The host
// serves lemond_url, the server the CLI used when enabling it.
std::string render_launch_agent_plist(const std::string& host_exe, const std::string& lemond_url);

// The drop-in `lemonade agents enable` writes for the systemd user unit, so the
// host serves lemond_url instead of the default port.
std::string render_systemd_dropin(const std::string& host_exe, const std::string& lemond_url);

bool is_lemonade_shim(const std::filesystem::path& file);
ShimState shim_state(const std::filesystem::path& file);
const char* shim_state_name(ShimState state);

// Writes the shim unless the file is foreign; leaves an identical shim alone.
// Returns false when a foreign file is in the way. Throws on write failures.
bool write_shim(const std::filesystem::path& file, const std::string& content);

// Removes the shim if it is ours. Returns whether a file was removed; error
// is set when ours couldn't be.
bool remove_shim(const std::filesystem::path& file, std::error_code* error = nullptr);

// The absolute directories of path_value (PATH syntax for this OS), in order.
std::vector<std::filesystem::path> path_dirs(const std::string& path_value);

// Every executable named `name` on path_value (PATH syntax for this OS), in
// order, as absolute paths. On Windows each PATHEXT extension is tried.
std::vector<std::filesystem::path> path_matches(const std::string& name,
                                                const std::string& path_value,
                                                const std::string& pathext = "");

} // namespace lemon::agents

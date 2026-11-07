#include "lemon/agents/agent_installer.h"

#include <algorithm>
#include <fstream>
#include <vector>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>

#include "lemon/backends/install_staging.h"
#include "lemon/utils/archive_platform.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/path_utils.h"

#ifdef __APPLE__
#include <libproc.h>
#endif

namespace fs = std::filesystem;

namespace lemon::agents {

namespace {

constexpr const char* kManifest = ".lemonade-install.json";

fs::path binary_in(const fs::path& dir, const std::string& binary) {
#ifdef _WIN32
    return dir / (binary + ".exe");
#else
    return dir / binary;
#endif
}

nlohmann::json read_manifest(const fs::path& dir) {
    std::ifstream in(dir / kManifest);
    nlohmann::json manifest = nlohmann::json::parse(in, nullptr, false);
    return manifest.is_object() ? manifest : nlohmann::json();
}

bool manifest_matches(const fs::path& dir, const AgentDescriptor& agent, const AgentPin& pin) {
    const nlohmann::json manifest = read_manifest(dir);
    std::error_code ec;
    return manifest.is_object() && manifest.value("agent", "") == agent.id &&
           manifest.value("version", "") == pin.version &&
           manifest.value("asset", "") == pin.asset &&
           manifest.value("sha256", "") == pin.sha256 &&
           fs::is_regular_file(binary_in(dir, agent.binary), ec);
}

// The extractor writes below the staging directory, but a symlink in the
// archive could still point out of it. A target is only allowed when it is
// relative with no `..`: checked lexically, a chain of links can climb out.
void check_contained(const fs::path& staging) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(staging, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_symlink(ec)) {
            continue;
        }
        const fs::path target = fs::read_symlink(it->path(), ec);
        bool climbs = false;
        for (const auto& part : target) {
            climbs = climbs || part == "..";
        }
        if (ec || target.is_absolute() || target.has_root_name() || climbs) {
            throw AgentError(500, "install_failed",
                             "The archive has an entry outside its directory: " +
                                 it->path().filename().string());
        }
    }
    if (ec) {
        throw AgentError(500, "install_failed", "Can't read the extracted files: " + ec.message());
    }
}

int compare_versions(const std::string& a, const std::string& b) {
    std::stringstream sa(a), sb(b);
    std::string pa, pb;
    while (true) {
        const bool ha = static_cast<bool>(std::getline(sa, pa, '.'));
        const bool hb = static_cast<bool>(std::getline(sb, pb, '.'));
        if (!ha && !hb) {
            return 0;
        }
        long na = 0, nb = 0;
        try { na = ha ? std::stol(pa) : 0; } catch (...) {}
        try { nb = hb ? std::stol(pb) : 0; } catch (...) {}
        if (na != nb) {
            return na < nb ? -1 : 1;
        }
    }
}

std::vector<fs::path> list_dir(const fs::path& dir) {
    std::vector<fs::path> entries;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        entries.push_back(it->path());
    }
    return entries;
}

// A removal is moved aside to a fresh name, since an interrupted one may
// have left the plain name behind.
fs::path aside_path(const fs::path& root, const std::string& name) {
    return root / (".removing-" + name + "-" +
                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

void remove_quietly(const fs::path& dir, std::vector<std::string>* warnings) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (ec && warnings != nullptr) {
        warnings->push_back("Can't remove " + dir.string() + " yet (" + ec.message() +
                            "); the next install retries.");
    }
}

} // namespace

std::optional<InstalledAgent> find_installed(const AgentDescriptor& agent,
                                             const AgentPaths& paths,
                                             const std::string& version) {
    std::optional<InstalledAgent> best;
    std::error_code ec;
    for (auto it = fs::directory_iterator(paths.agent_dir(agent.id), ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_directory(ec)) {
            continue;
        }
        const nlohmann::json manifest = read_manifest(it->path());
        if (!manifest.is_object() || manifest.value("agent", "") != agent.id) {
            continue;
        }
        const std::string found = manifest.value("version", "");
        const fs::path binary = binary_in(it->path(), agent.binary);
        if (found.empty() || it->path().filename() != found || !fs::is_regular_file(binary, ec)) {
            continue;
        }
        if (!version.empty() && found != version) {
            continue;
        }
        if (!best || compare_versions(found, best->version) > 0) {
            best = InstalledAgent{found, it->path(), binary};
        }
    }
    return best;
}

void VersionLeases::acquire(const fs::path& dir) {
    leases_[dir].launches++;
}

bool VersionLeases::held(const fs::path& dir) {
    const auto it = leases_.find(dir);
    return it != leases_.end() &&
           (it->second.launches > 0 || it->second.until > std::chrono::steady_clock::now());
}

void VersionLeases::release(const fs::path& dir, std::chrono::seconds grace) {
    std::lock_guard<std::mutex> lock(mutex_);
    Lease& lease = leases_[dir];
    lease.launches = std::max(lease.launches - 1, 0);
    lease.until = std::max(lease.until, std::chrono::steady_clock::now() + grace);
}

std::optional<InstalledAgent> find_active(const AgentDescriptor& agent, const AgentPaths& paths,
                                          const std::string& pinned_version) {
    if (!pinned_version.empty()) {
        if (auto pinned = find_installed(agent, paths, pinned_version)) {
            return pinned;
        }
    }
    return find_installed(agent, paths);
}

std::string rendered_shim(const AgentDescriptor& agent, const fs::path& binary) {
#ifdef _WIN32
    return render_cmd_shim(agent.id, binary,
                           {{"LOCALAPPDATA",
                             lemon::utils::get_environment_variable_utf8("LOCALAPPDATA")}});
#else
    return render_posix_shim(agent.id, binary);
#endif
}

// The line of a rendered shim that runs the binary.
std::string command_line(const std::string& shim) {
    std::stringstream lines(shim);
    for (std::string line; std::getline(lines, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("exec ", 0) == 0 || line.find(" %*") != std::string::npos) {
            return line;
        }
    }
    return "";
}

// The installed version our shim runs, from the line that runs it, so a shim
// from another Lemonade build still counts.
std::optional<InstalledAgent> shim_target(const AgentDescriptor& agent, const AgentPaths& paths) {
    const fs::path shim = shim_path(paths, agent.binary);
    if (shim_state(shim) != ShimState::Ours) {
        return std::nullopt;
    }
    std::ifstream in(shim, std::ios::binary);
    std::stringstream content;
    content << in.rdbuf();
    const std::string text = content.str();
    for (const fs::path& entry : list_dir(paths.agent_dir(agent.id))) {
        auto installed = find_installed(agent, paths, entry.filename().string());
        if (!installed) {
            continue;
        }
        const std::string runs = command_line(rendered_shim(agent, installed->binary));
        // Earlier .cmd shims named the binary by its absolute path.
        const std::string absolute = "\"" + lemon::utils::path_to_utf8(installed->binary) + "\" %*";
        if ((!runs.empty() && text.find(runs) != std::string::npos) ||
            text.find(absolute) != std::string::npos) {
            return installed;
        }
    }
    return std::nullopt;
}

// Whether a process of this user runs an executable from dir.
bool runs_from(const fs::path& dir) {
    // The kernel reports executables by their resolved path.
    std::error_code canonical_ec;
    const fs::path resolved = fs::weakly_canonical(dir, canonical_ec);
    const std::string prefix =
        lemon::utils::path_to_utf8(canonical_ec ? dir : resolved) + "/";
#if defined(__linux__)
    std::error_code ec;
    for (auto it = fs::directory_iterator("/proc", ec); !ec && it != fs::directory_iterator();
         it.increment(ec)) {
        const std::string pid = it->path().filename().string();
        if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos) {
            continue;
        }
        std::error_code link_ec;
        const fs::path exe = fs::read_symlink(it->path() / "exe", link_ec);
        if (!link_ec && exe.string().rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
#elif defined(__APPLE__)
    // proc_listallpids returns a count of pids; a full buffer may be truncated.
    std::vector<pid_t> pids(std::max(proc_listallpids(nullptr, 0), 0) + 256);
    int count = 0;
    while (true) {
        count = proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
        if (count < static_cast<int>(pids.size())) {
            break;
        }
        pids.resize(pids.size() * 2);
    }
    for (int i = 0; i < count; ++i) {
        char path[PROC_PIDPATHINFO_MAXSIZE];
        if (proc_pidpath(pids[i], path, sizeof(path)) > 0 &&
            std::string(path).rfind(prefix, 0) == 0) {
            return true;
        }
    }
    return false;
#else
    // Windows refuses to rename a directory with a running executable, which
    // the removal relies on instead.
    (void)prefix;
    return false;
#endif
}

InstallOutcome install_agent(const AgentDescriptor& agent, const AgentPin& pin,
                             const AgentPaths& paths, bool force,
                             const InstallProgress& progress,
                             const lemon::utils::CancelToken& cancel,
                             lemon::utils::HttpSecurityPolicy policy,
                             VersionLeases* leases) {
    InstallOutcome outcome;
    const fs::path root = paths.agent_dir(agent.id);
    const fs::path dir = root / pin.version;
    std::error_code ec;

    if (force || !manifest_matches(dir, agent, pin)) {
        fs::create_directories(root, ec);
        if (ec) {
            throw AgentError(500, "install_failed", "Can't create " + root.string() + ": " +
                                                        ec.message());
        }
        // download_file keeps its own temp files next to the target, so the
        // whole directory goes when the install ends.
        const fs::path download_dir = root / ".download";
        fs::create_directories(download_dir, ec);
        const fs::path download = download_dir / pin.asset;
        struct DirGuard {
            fs::path path;
            ~DirGuard() {
                std::error_code ignored;
                fs::remove_all(path, ignored);
            }
        } download_guard{download_dir};

        lemon::utils::DownloadOptions options;
        options.expected_hash = pin.sha256;
        // download_file retries a checksum mismatch from scratch too, which a
        // pinned asset rarely fixes, so retries stay few.
        options.max_retries = 2;
        const auto result = lemon::utils::HttpClient::download_file(
            pin.url, lemon::utils::path_to_utf8(download),
            [&progress](size_t done, size_t total) { return !progress || progress(done, total); },
            {}, options, policy, cancel);
        if (!result.success) {
            if (result.cancelled) {
                throw AgentError(499, "cancelled", "The install was cancelled.");
            }
            if (result.error_message.find("hash mismatch") != std::string::npos) {
                throw AgentError(502, "checksum_mismatch",
                                 pin.asset + " doesn't match its pinned checksum.");
            }
            throw AgentError(502, "download_failed",
                             "Can't download " + pin.url + ": " + result.error_message);
        }
        outcome.downloaded = true;

        const fs::path staging = dir.string() + ".staging";
        fs::remove_all(staging, ec);
        struct StagingGuard {
            fs::path path;
            ~StagingGuard() {
                std::error_code ignored;
                fs::remove_all(path, ignored);
            }
        } staging_guard{staging};
        fs::create_directories(staging, ec);

        auto archive = lemon::utils::create_archive_platform();
        const std::string archive_path = lemon::utils::path_to_utf8(download);
        const std::string staging_path = lemon::utils::path_to_utf8(staging);
        const bool is_zip = pin.asset.size() > 4 &&
                            pin.asset.compare(pin.asset.size() - 4, 4, ".zip") == 0;
        const bool extracted = is_zip
                                   ? archive->extract_zip(archive_path, staging_path, agent.id)
                                   : archive->extract_tarball(archive_path, staging_path, agent.id);
        if (!extracted) {
            throw AgentError(500, "install_failed", "Can't extract " + pin.asset + ".");
        }
        check_contained(staging);
        const fs::path staged_binary = binary_in(staging, agent.binary);
        if (!fs::is_regular_file(staged_binary, ec)) {
            throw AgentError(500, "install_failed",
                             pin.asset + " has no " + staged_binary.filename().string() + ".");
        }
        fs::permissions(staged_binary,
                        fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                            fs::perms::others_read | fs::perms::others_exec,
                        ec);
        {
            std::ofstream out(staging / kManifest);
            out << nlohmann::json{{"agent", agent.id},
                                  {"version", pin.version},
                                  {"asset", pin.asset},
                                  {"sha256", pin.sha256},
                                  {"binary", staged_binary.filename().string()}}
                       .dump(2)
                << "\n";
            if (!out) {
                throw AgentError(500, "install_failed", "Can't write the install manifest.");
            }
        }

        try {
            if (lemon::backends::commit_staged_install(staging_path,
                                                       lemon::utils::path_to_utf8(dir),
                                                       agent.binary)
                    .empty()) {
                throw AgentError(500, "install_failed", "The staged install has no binary.");
            }
        } catch (const std::runtime_error& e) {
            if (dynamic_cast<const AgentError*>(&e) != nullptr) {
                throw;
            }
#ifdef _WIN32
            throw AgentError(409, "agent_busy",
                             agent.name + " " + pin.version +
                                 " is running, so its files can't be replaced. Close it and retry.");
#else
            throw AgentError(500, "install_failed", e.what());
#endif
        }
    }

    outcome.installed = InstalledAgent{pin.version, dir, binary_in(dir, agent.binary)};

    const fs::path shim = shim_path(paths, agent.binary);
    const std::string content = rendered_shim(agent, outcome.installed.binary);
    try {
        if (!write_shim(shim, content)) {
            outcome.warnings.push_back(shim.string() + " isn't Lemonade's, so it was left alone; `" +
                                       agent.binary + "` there doesn't run the managed " +
                                       agent.name + ".");
        }
    } catch (const std::exception& e) {
        outcome.warnings.push_back(std::string("Can't write the shim: ") + e.what());
    }
    outcome.shim = shim_state(shim);

    // Other versions go unless a pi still runs from one or the shim still
    // names it (it could not be rewritten). A directory is renamed aside first: Windows refuses that while
    // its executable runs, so a running version is never half deleted. Leftovers
    // of an interrupted removal go on the next install.
    std::unique_lock<std::mutex> retiring;
    if (leases != nullptr) {
        retiring = std::unique_lock<std::mutex>(leases->mutex());
    }
    const auto shim_runs = shim_target(agent, paths);
    std::vector<fs::path> retired;
    for (const fs::path& entry : list_dir(root)) {
        const std::string name = entry.filename().string();
        if (name.rfind(".removing-", 0) == 0) {
            retired.push_back(entry);
            continue;
        }
        if (entry == dir || name[0] == '.' || !fs::is_directory(entry, ec) ||
            (shim_runs && entry == shim_runs->dir) || (leases && leases->held(entry)) ||
            runs_from(entry)) {
            continue;
        }
        const fs::path aside = aside_path(root, name);
        fs::rename(entry, aside, ec);
        if (!ec) {
            retired.push_back(aside);
        }
    }
    // Launches only wait for the rename, not for the payload to be deleted.
    if (retiring) {
        retiring.unlock();
    }
    for (const fs::path& aside : retired) {
        remove_quietly(aside, nullptr);
    }
    return outcome;
}

bool uninstall_agent(const AgentDescriptor& agent, const AgentPaths& paths,
                     std::vector<std::string>& warnings, VersionLeases* leases) {
    // Puts versions moved aside back, or says which one it couldn't: a
    // later install would otherwise delete it as a leftover.
    const auto put_back = [&agent](const std::vector<std::pair<fs::path, fs::path>>& moved) {
        std::string stuck;
        for (const auto& [from, to] : moved) {
            std::error_code back_ec;
            fs::rename(to, from, back_ec);
            if (back_ec) {
                stuck += (stuck.empty() ? "" : ", ") + from.filename().string() + " at " +
                         to.string();
            }
        }
        if (!stuck.empty()) {
            throw AgentError(500, "uninstall_failed",
                             "Uninstalling " + agent.name + " stopped partway; left aside: " +
                                 stuck + ". Run `lemonade agents uninstall " + agent.id +
                                 "` again.");
        }
    };
    const fs::path root = paths.agent_dir(agent.id);
    std::error_code ec;
    std::vector<fs::path> retired;
    bool removed = false;
    {
        std::unique_lock<std::mutex> retiring;
        if (leases != nullptr) {
            retiring = std::unique_lock<std::mutex>(leases->mutex());
        }
        // Nothing changes unless the versions can be listed and the shim inspected.
        std::vector<fs::path> entries;
        for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator();
             it.increment(ec)) {
            entries.push_back(it->path());
        }
        if (ec && ec != std::errc::no_such_file_or_directory) {
            throw AgentError(500, "uninstall_failed",
                             "Can't list " + root.string() + ": " + ec.message());
        }
        ec.clear();
        const fs::path shim = shim_path(paths, agent.binary);
        // An unreadable shim directory can't be told apart from no shim.
        (void)fs::symlink_status(shim, ec);
        if (ec && ec != std::errc::no_such_file_or_directory) {
            throw AgentError(500, "uninstall_failed",
                             "Can't inspect " + shim.string() + ": " + ec.message());
        }
        ec.clear();
        // Half an uninstall would leave pi looking installed, so a launch in
        // progress makes it wait for the whole thing.
        for (const fs::path& entry : entries) {
            if (leases != nullptr && leases->held(entry)) {
                throw AgentError(409, "agent_busy",
                                 agent.name + " is being launched; uninstall it again in a minute.");
            }
        }
        // All versions move aside or none do: a version Windows keeps open
        // (its executable runs) puts back the ones already moved.
        std::vector<std::pair<fs::path, fs::path>> moved;
        for (const fs::path& entry : entries) {
            const std::string name = entry.filename().string();
            if (name[0] == '.') {
                retired.push_back(entry);  // leftovers and downloads
                continue;
            }
            const fs::path aside = aside_path(root, name);
            fs::rename(entry, aside, ec);
            if (ec) {
                const std::string reason = ec.message();
                put_back(moved);
                throw AgentError(409, "agent_busy",
                                 "Can't remove " + agent.name + " " + name + " while it's in use (" +
                                     reason + "). Close it and uninstall again.");
            }
            moved.emplace_back(entry, aside);
        }
        std::error_code shim_ec;
        const bool shim_removed = remove_shim(shim, &shim_ec);
        if (shim_ec) {
            put_back(moved);
            throw AgentError(500, "uninstall_failed",
                             "Can't remove " + shim.string() + ": " + shim_ec.message());
        }
        for (const auto& [from, to] : moved) {
            retired.push_back(to);
        }
        removed = !moved.empty() || shim_removed;
    }
    // Launches only wait for the renames, not for the payloads to be deleted.
    for (const fs::path& aside : retired) {
        remove_quietly(aside, &warnings);
    }
    if (fs::exists(root, ec) && fs::is_empty(root, ec)) {
        fs::remove(root, ec);
    }
    return removed;
}

} // namespace lemon::agents

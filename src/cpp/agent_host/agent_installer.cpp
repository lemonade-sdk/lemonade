#include "lemon/agents/agent_installer.h"

#include <fstream>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>

#include "lemon/backends/install_staging.h"
#include "lemon/utils/archive_platform.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/path_utils.h"

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

bool is_inside(const fs::path& root, const fs::path& candidate) {
    const fs::path rel = candidate.lexically_normal().lexically_relative(root.lexically_normal());
    return !rel.empty() && *rel.begin() != "..";
}

// The extractor writes below the staging directory, but a symlink in the
// archive could still point out of it.
void check_contained(const fs::path& staging) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(staging, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_symlink(ec)) {
            continue;
        }
        const fs::path target = fs::read_symlink(it->path(), ec);
        if (ec || target.is_absolute() ||
            !is_inside(staging, it->path().parent_path() / target)) {
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

InstallOutcome install_agent(const AgentDescriptor& agent, const AgentPin& pin,
                             const AgentPaths& paths, bool force,
                             const InstallProgress& progress,
                             const lemon::utils::CancelToken& cancel,
                             lemon::utils::HttpSecurityPolicy policy) {
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
        const fs::path download = root / (".download-" + pin.asset);
        struct FileGuard {
            fs::path path;
            ~FileGuard() {
                std::error_code ignored;
                fs::remove(path, ignored);
                fs::remove(path.string() + ".partial", ignored);
            }
        } download_guard{download};

        lemon::utils::DownloadOptions options;
        options.expected_hash = pin.sha256;
        // download_file retries a mismatch from scratch, which a pinned asset won't fix.
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
        struct DirGuard {
            fs::path path;
            ~DirGuard() {
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
#ifdef _WIN32
    const std::string content = render_cmd_shim(agent.id, outcome.installed.binary);
#else
    const std::string content = render_posix_shim(agent.id, outcome.installed.binary);
#endif
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

    // Older versions may still be running; on Windows their files stay
    // locked, so removal is best effort and retried on the next install.
    for (const fs::path& entry : list_dir(root)) {
        if (entry != dir && entry.filename().string().rfind(".download-", 0) != 0 &&
            fs::is_directory(entry, ec)) {
            remove_quietly(entry, nullptr);
        }
    }
    return outcome;
}

bool uninstall_agent(const AgentDescriptor& agent, const AgentPaths& paths,
                     std::vector<std::string>& warnings) {
    bool removed = remove_shim(shim_path(paths, agent.binary));
    const fs::path root = paths.agent_dir(agent.id);
    std::error_code ec;
    for (const fs::path& entry : list_dir(root)) {
        removed = true;
        remove_quietly(entry, &warnings);
    }
    if (fs::exists(root, ec) && fs::is_empty(root, ec)) {
        fs::remove(root, ec);
    }
    return removed;
}

} // namespace lemon::agents

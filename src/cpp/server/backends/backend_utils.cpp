#include "lemon/backends/backend_utils.h"
#include "lemon/backends/install_staging.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/backends/backend_registry.h"  // spec_for() — descriptor->install spec, no server includes
#include "lemon/model_manager.h"  // For DownloadProgress, DownloadProgressCallback

#include "lemon/utils/path_utils.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/archive_platform.h"
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <lemon/utils/aixlog.hpp>
#include <algorithm>
#include <system_error>
#include <vector>
#include <nlohmann/json.hpp>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <unistd.h>
    #include <sys/stat.h>
#endif

using json = nlohmann::json;

namespace lemon::backends {

    const BackendSpec* try_get_spec_for_recipe(const std::string& recipe) {
        // Each backend exposes its install/download spec through the registry.
        return spec_for(recipe);
    }

    static std::string hash_string_from_json(const json& node) {
        if (node.is_string()) {
            return node.get<std::string>();
        }
        if (!node.is_object()) {
            return "";
        }
        if (node.contains("digest") && node["digest"].is_string()) {
            return node["digest"].get<std::string>();
        }
        if (node.contains("sha256") && node["sha256"].is_string()) {
            return "sha256:" + node["sha256"].get<std::string>();
        }
        if (node.contains("algorithm") && node["algorithm"].is_string() &&
            node.contains("value") && node["value"].is_string()) {
            return node["algorithm"].get<std::string>() + ":" + node["value"].get<std::string>();
        }
        return "";
    }

    static std::string lookup_hash_path(const json& root, const std::vector<std::string>& path) {
        const json* current = &root;
        for (const auto& part : path) {
            if (!current->is_object() || !current->contains(part)) {
                return "";
            }
            current = &((*current)[part]);
        }
        return hash_string_from_json(*current);
    }

    static std::string lookup_expected_asset_hash(const std::string& recipe,
                                                  const std::string& backend,
                                                  const std::string& version,
                                                  const std::string& repo,
                                                  const std::string& filename) {
        try {
            const std::string config_path = utils::get_resource_path("resources/backend_versions.json");
            const json config = utils::JsonUtils::load_from_file(config_path);

            const std::vector<std::vector<std::string>> candidate_paths = {
                {"checksums", "github", repo, version, filename},
                {"checksums", repo, version, filename},
                {"checksums", recipe, backend, version, filename},
                {"checksums", recipe, version, filename},
                {"checksums", recipe, filename},
                {"artifact_hashes", "github", repo, version, filename},
                {"artifact_hashes", repo, version, filename},
                {"artifact_hashes", recipe, backend, version, filename},
                {"artifact_hashes", recipe, version, filename},
                {"artifact_hashes", filename}
            };

            for (const auto& path : candidate_paths) {
                // Skip repo-keyed shapes when no repo is available (e.g. TheRock).
                if (std::find(path.begin(), path.end(), std::string()) != path.end()) {
                    continue;
                }
                std::string hash = lookup_hash_path(config, path);
                if (!hash.empty()) {
                    return hash;
                }
            }
        } catch (const std::exception& e) {
            LOG(DEBUG, "BackendUtils") << "Could not load backend artifact checksums: "
                                       << e.what() << std::endl;
        }
        return "";
    }

    bool BackendUtils::extract_zip(const std::string& zip_path, const std::string& dest_dir, const std::string& backend_name) {
        auto archive_platform = utils::create_archive_platform();
        return archive_platform->extract_zip(zip_path, dest_dir, backend_name);
    }

    bool BackendUtils::extract_tarball(const std::string& tarball_path, const std::string& dest_dir, const std::string& backend_name) {
        auto archive_platform = utils::create_archive_platform();
        return archive_platform->extract_tarball(tarball_path, dest_dir, backend_name);
    }

    static bool ends_with(const std::string& s, const std::string& suffix) {
        return s.size() >= suffix.size() &&
            s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    static bool is_tarball(const std::string& filename) {
        // Any tar variant we know how to feed to `tar -xf`.
        return ends_with(filename, ".tar.gz") ||
               ends_with(filename, ".tgz") ||
               ends_with(filename, ".tar.xz") ||
               ends_with(filename, ".txz") ||
               ends_with(filename, ".tar.bz2") ||
               ends_with(filename, ".tbz2");
    }

    static bool is_seven_zip(const std::string& filename) {
        return ends_with(filename, ".7z");
    }

    // Greedy glob match where '*' matches any (possibly empty) run of
    // characters. No '?' support — release asset names only need '*'.
    static bool wildcard_match(const std::string& pattern, const std::string& text) {
        size_t p = 0, t = 0, star = std::string::npos, mark = 0;
        while (t < text.size()) {
            if (p < pattern.size() && pattern[p] == '*') {
                star = p++;
                mark = t;
            } else if (p < pattern.size() && pattern[p] == text[t]) {
                ++p;
                ++t;
            } else if (star != std::string::npos) {
                p = star + 1;
                t = ++mark;
            } else {
                return false;
            }
        }
        while (p < pattern.size() && pattern[p] == '*') {
            ++p;
        }
        return p == pattern.size();
    }

    // Resolve a '*' wildcard in a release asset filename to the concrete asset
    // name published for `tag`. Some upstreams embed a component that changes
    // on every build (e.g. the macOS runner version in sd-cpp's Darwin asset:
    // sd-...-bin-Darwin-macOS-15.7.7-arm64.zip). Rather than hardcode and chase
    // that value on every bump, the backend spec carries a '*' placeholder and
    // we look up the real asset name here via the GitHub Releases API. Returns
    // the pattern unchanged when it contains no wildcard.
    static std::string resolve_asset_wildcard(const std::string& repo,
                                              const std::string& tag,
                                              const std::string& pattern,
                                              const BackendSpec& spec) {
        if (pattern.find('*') == std::string::npos) {
            return pattern;
        }

        const std::string url = "https://api.github.com/repos/" + repo +
                                "/releases/tags/" + tag;
        const std::map<std::string, std::string> headers = {
            {"User-Agent", "lemonade"},
            {"Accept", "application/vnd.github+json"},
        };

        LOG(DEBUG, spec.log_name()) << "Resolving asset wildcard '" << pattern
            << "' for " << repo << "@" << tag << " via " << url << std::endl;

        utils::HttpResponse resp;
        try {
            resp = utils::HttpClient::get(url, headers);
        } catch (const std::exception& e) {
            throw std::runtime_error(
                "Failed to query GitHub for release '" + tag + "' of " + repo +
                " to resolve asset '" + pattern + "': " + e.what());
        }
        if (resp.status_code < 200 || resp.status_code >= 300) {
            throw std::runtime_error(
                "GitHub returned HTTP " + std::to_string(resp.status_code) +
                " when resolving asset '" + pattern + "' for " + repo + "@" + tag);
        }

        json body;
        try {
            body = json::parse(resp.body);
        } catch (const std::exception& e) {
            throw std::runtime_error(
                "Failed to parse GitHub release response for " + repo + "@" +
                tag + ": " + e.what());
        }

        if (body.contains("assets") && body["assets"].is_array()) {
            for (const auto& asset : body["assets"]) {
                if (!asset.contains("name") || !asset["name"].is_string()) {
                    continue;
                }
                const std::string name = asset["name"].get<std::string>();
                if (wildcard_match(pattern, name)) {
                    LOG(INFO, spec.log_name()) << "Resolved asset wildcard '"
                        << pattern << "' to '" << name << "'" << std::endl;
                    return name;
                }
            }
        }

        throw std::runtime_error(
            "No release asset matching '" + pattern + "' found for " + repo +
            "@" + tag);
    }

    bool BackendUtils::extract_seven_zip(const std::string& archive_path, const std::string& dest_dir, const std::string& backend_name) {
        // CUDA Windows release assets are .7z and use the existing native tar.exe path.
        // Linux CUDA assets are .tar.xz, so Linux should not require bsdtar/7z/p7zip.
        fs::create_directories(dest_dir);
        LOG(DEBUG, backend_name) << "Extracting 7z to " << dest_dir << std::endl;
#ifdef _WIN32
        auto platform = lemon::utils::create_archive_platform();

        // Windows System32\tar.exe is bsdtar (libarchive) on Windows 11 22H2+,
        // which can read .7z. Probe with `--list` first to confirm .7z support;
        // older tar.exe (from Windows 10) will exit non-zero for .7z archives.
        if (!platform->is_native_tar_available()) {
            LOG(ERROR, backend_name) << "Error: 'tar' command not found. Windows 11 22H2+ required for .7z support." << std::endl;
            return false;
        }
        {
            std::string tar_path = platform->get_native_tar_path();
            int probe_result = lemon::utils::ProcessManager::run_process_with_output(
                tar_path,
                {"--list", "-f", archive_path},
                [](const std::string&) { return true; },
                "",
                10
            );
            if (probe_result != 0) {
                LOG(ERROR, backend_name) << "Error: tar.exe cannot read this .7z archive. Windows 11 22H2+ (bsdtar/libarchive) required." << std::endl;
                return false;
            }
        }
        // Note: do NOT use --strip-components=1 here. The CUDA .7z archives from
        // lemonade-sdk/llama.cpp have no top-level directory — files sit at the
        // archive root. Stripping would discard every entry and produce an empty dir.
        std::string output;
        int result = lemon::utils::ProcessManager::run_process_with_output(
            platform->get_native_tar_path(),
            {"-xf", archive_path, "-C", dest_dir},
            [&output](const std::string& line) {
                output += line + "\n";
                return true;
            },
            "",
            300
        );
        if (result != 0) {
            LOG(ERROR, backend_name) << "Extraction failed with code: " << result
                                     << (output.empty() ? "" : " - " + output) << std::endl;
            return false;
        }
        return true;
#else
        LOG(ERROR, backend_name) << "Error: .7z backend archives are only expected on Windows. Linux CUDA assets should be .tar.xz." << std::endl;
        return false;
#endif
    }

    static fs::path get_backend_download_cache_dir() {
        fs::path cache_dir = fs::path(utils::get_downloaded_bin_dir()) / ".downloads";
        fs::create_directories(cache_dir);
        return cache_dir;
    }

    // Helper to extract archive files based on extension
    bool BackendUtils::extract_archive(const std::string& archive_path, const std::string& dest_dir, const std::string& backend_name) {
        if (is_tarball(archive_path)) {
            return extract_tarball(archive_path, dest_dir, backend_name);
        }
        if (is_seven_zip(archive_path)) {
            return extract_seven_zip(archive_path, dest_dir, backend_name);
        }
        // Default to ZIP extraction
        return extract_zip(archive_path, dest_dir, backend_name);
    }

    std::string BackendUtils::get_install_directory(const std::string& dir_name, const std::string& backend) {
        // Use fs::path throughout to ensure consistent native separators
        fs::path ret = fs::path(utils::get_downloaded_bin_dir()) / dir_name;
        if (!backend.empty()) ret /= backend;
        return ret.make_preferred().string();
    }

    void BackendUtils::build_bin_config_key(const std::string& recipe,
                                              const std::string& backend,
                                              std::string& out_section,
                                              std::string& out_bin_key) {
        std::string config_backend = backend;
        if ((recipe_has_rocm_channels(recipe) &&
            (backend == "rocm-stable" || backend == "rocm-nightly"))) {
            config_backend = "rocm";
        }
        out_section = RuntimeConfig::recipe_to_config_section(recipe);
        out_bin_key = config_backend.empty() ? "server_bin" : (config_backend + "_bin");
    }

    std::string BackendUtils::find_external_backend_binary(const std::string& recipe, const std::string& backend) {
        auto* cfg = lemon::RuntimeConfig::global();
        if (!cfg) return "";

        std::string section, bin_key;
        build_bin_config_key(recipe, backend, section, bin_key);
        std::string bin_value = cfg->backend_string(section, bin_key);

        // Reserved keywords and bare version tags are handled by the install flow.
        if (bin_value.empty() || bin_value == "builtin" || bin_value == "latest") {
            return "";
        }
        if (!utils::looks_like_path(bin_value)) {
            return "";
        }

        RuntimeConfig::validate_bin_path(section, bin_key, bin_value);
        return bin_value;
    }

    std::string BackendUtils::get_bin_config_value(const std::string& recipe, const std::string& backend) {
        auto* cfg = lemon::RuntimeConfig::global();
        if (!cfg) return "";
        std::string section, bin_key;
        build_bin_config_key(recipe, backend, section, bin_key);
        return cfg->backend_string(section, bin_key);
    }

    std::string BackendUtils::find_executable_in_install_dir(const std::string& install_dir, const std::string& binary_name) {
        // Delegates to the header-only helper so the executable-lookup logic has
        // a single source of truth shared with commit_staged_install().
        return find_executable_in_dir(install_dir, binary_name);
    }

    std::string BackendUtils::get_backend_binary_path(const BackendSpec& spec, const std::string& backend) {
        if (backend == "system") {
            // Check if binary exists in PATH
            std::string path = utils::find_executable_in_path(spec.binary);
            if (!path.empty()) {
                return spec.binary;
            }
            throw std::runtime_error(spec.binary + " not found in PATH");
        }

        // Resolve "rocm" to actual channel for backends that support ROCm channels
        std::string resolved_backend = backend;
        if (recipe_has_rocm_channels(spec.recipe) && backend == "rocm") {
            std::string channel = "stable";  // default to stable
            if (auto* cfg = RuntimeConfig::global()) {
                channel = cfg->rocm_channel_for_recipe(spec.recipe);
            }
            resolved_backend = "rocm-" + channel;
        }

        std::string exe_path = find_external_backend_binary(spec.recipe, resolved_backend);

        if (!exe_path.empty()) {
            return exe_path;
        }

        std::string install_dir = get_install_directory(spec.recipe, resolved_backend);
        exe_path = find_executable_in_install_dir(install_dir, spec.binary);

        if (!exe_path.empty()) {
            return exe_path;
        }

        // If not found, throw error with helpful message
        throw std::runtime_error(spec.binary + " not found in install directory: " + install_dir);
    }

    static std::string get_version_file(std::string& install_dir) {
        return (fs::path(install_dir) / "version.txt").string();
    }

    std::string BackendUtils::get_installed_version_file(const BackendSpec& spec, const std::string& backend) {
        if (backend == "system") {
            return "";
        }

        // Normalize the public rocm backend name to the configured channel before
        // reading version.txt. get_backend_binary_path() already does this when
        // locating the executable; version detection must use the same install
        // directory or ROCm backends remain stuck in update_required after a
        // successful install.
        std::string resolved_backend = backend;
        if (recipe_has_rocm_channels(spec.recipe) && backend == "rocm") {
            std::string channel = "stable";
            if (auto* cfg = RuntimeConfig::global()) {
                channel = cfg->rocm_channel_for_recipe(spec.recipe);
            }
            resolved_backend = "rocm-" + channel;
        }

        std::string install_dir = get_install_directory(spec.recipe, resolved_backend);
        return get_version_file(install_dir);
    }

    std::string BackendUtils::get_backend_version(const std::string& recipe, const std::string& backend) {
        std::string resolved_backend = backend;
        if (recipe_has_rocm_channels(recipe) && backend == "rocm") {
            // Map "rocm" to the appropriate channel based on config
            std::string channel = "stable";  // default to stable for now
            if (auto* cfg = RuntimeConfig::global()) {
                channel = cfg->rocm_channel_for_recipe(recipe);
            }
            resolved_backend = "rocm-" + channel;
        }

        std::string config_path = utils::get_resource_path("resources/backend_versions.json");

        json config = utils::JsonUtils::load_from_file(config_path);

        if (!config.contains(recipe) || !config[recipe].is_object()) {
            throw std::runtime_error("backend_versions.json is missing '" + recipe + "' section");
        }

        const auto& recipe_config = config[recipe];
        const std::string backend_id = recipe + ":" + resolved_backend;

        if (!recipe_config.contains(resolved_backend) || !recipe_config[resolved_backend].is_string()) {
            throw std::runtime_error("backend_versions.json is missing version for backend: " + backend_id);
        }

        std::string version = recipe_config[resolved_backend].get<std::string>();
        return version;
    }

    void BackendUtils::install_from_github(const BackendSpec& spec,
                                           const std::string& expected_version,
                                           const std::string& repo,
                                           const std::string& asset_pattern,
                                           const std::string& backend,
                                           DownloadProgressCallback progress_cb) {
        std::string install_dir;
        std::string version_file;
        std::string exe_path = find_external_backend_binary(spec.recipe, backend);
        bool needs_install = exe_path.empty();

        if (needs_install) {
            install_dir = get_install_directory(spec.recipe, backend);
            version_file = get_version_file(install_dir);

            // Check if already installed with correct version
            exe_path = find_executable_in_install_dir(install_dir, spec.binary);
            needs_install = exe_path.empty();

            if (!needs_install && fs::exists(version_file)) {
                std::string installed_version;
                std::ifstream vf(version_file);
                std::getline(vf, installed_version);
                vf.close();

                if (installed_version != expected_version) {
                    LOG(INFO, spec.log_name()) << "Upgrading " << spec.binary << " from " << installed_version
                            << " to " << expected_version << std::endl;
                    needs_install = true;
                    // NOTE: do NOT remove install_dir here. The existing working
                    // binary is kept in place until the replacement has been
                    // downloaded, extracted, and verified; the atomic swap below
                    // (commit_staged_install) handles removal so an interrupted
                    // download cannot leave the backend with no usable binary.
                }
            } else if (!needs_install && !expected_version.empty()) {
                // If the executable exists but version.txt is missing, SystemInfo
                // reports update_required because it cannot prove the installed
                // binary matches the current Lemonade baseline. Treat this like a
                // version mismatch rather than a 0B no-op completion.
                LOG(INFO, spec.log_name()) << "Installed executable is missing version.txt; reinstalling "
                        << spec.binary << " version " << expected_version << std::endl;
                needs_install = true;
                // See note above: removal is deferred to the verified atomic swap.
            }
        }

        if (needs_install) {
            LOG(INFO, spec.log_name()) << "Installing " << spec.binary << " (version: "
                    << expected_version << ")" << std::endl;

            // Resolve any '*' wildcard in the asset name (e.g. the macOS runner
            // version in sd-cpp's Darwin asset) to the concrete published name
            // before building any download URL. No-op when there is no wildcard.
            const std::string filename =
                resolve_asset_wildcard(repo, expected_version, asset_pattern, spec);

            // Stage the new install in a sibling directory so the currently
            // installed (working) binary is left untouched until the download is
            // complete, extracted, and verified. Only then is staging atomically
            // swapped into place (see commit_staged_install below), so a slow or
            // interrupted download never destroys a working binary.
            const std::string staging_dir = install_dir + ".staging";
            std::error_code staging_ec;
            fs::remove_all(staging_dir, staging_ec);  // clear any leftover from a prior aborted install
            fs::remove_all(install_dir + ".old", staging_ec);  // and any orphaned swap backup
            // If a stale staging tree could not be cleared (e.g. a locked file on
            // Windows), fail rather than extracting into it — a leftover binary
            // could otherwise satisfy verification and get promoted as a stale or
            // mixed install over the working one.
            if (fs::exists(staging_dir)) {
                throw std::runtime_error("Could not clear stale staging directory: " + staging_dir);
            }
            fs::create_directories(staging_dir);

            // Remove the staging tree on any early exit (exception) before the
            // swap, so a failed download/extraction never leaves a half-built
            // tree behind for the next attempt to trip over.
            struct StagingGuard {
                const std::string& dir;
                bool active = true;
                ~StagingGuard() {
                    if (active) {
                        std::error_code ec;
                        fs::remove_all(dir, ec);
                    }
                }
            } staging_guard{staging_dir};

            // Download archive to cache directory.
            // Preserve the actual filename (sanitised for use in a path) so that
            // extract_archive() dispatches to the correct extractor based on extension,
            // and architecture-specific assets (e.g. sm_86 vs sm_89) don't collide.
            fs::path cache_dir = fs::temp_directory_path();
            fs::create_directories(cache_dir);
            std::string zip_name = backend.empty() ? spec.recipe : spec.recipe + "_" + backend;
            std::string safe_filename = filename;
            for (char& ch : safe_filename) {
                if (ch == '/' || ch == '\\' || ch == ':') ch = '_';
            }
            std::string zip_path = (cache_dir / (zip_name + "_" + expected_version + "_" + safe_filename)).string();

            LOG(DEBUG, spec.log_name()) << "Downloading to: " << zip_path << std::endl;

            // Remove the downloaded archive on ANY exit from here on — success
            // OR exception, including a throw from commit_staged_install() below
            // (a swap/rename failure) — so the cache archive is never leaked.
            struct ZipGuard {
                const std::string& path;
                ~ZipGuard() {
                    std::error_code ec;
                    fs::remove(path, ec);
                }
            } zip_guard{zip_path};

            const std::string base_download_url = "https://github.com/" + repo + "/releases/download/" +
                                                  expected_version + "/";

            // Decide single-file vs. split-archive without hitting the GitHub
            // Releases API (which is rate-limited to 60 req/hr per IP, and
            // 5000/hr even with a token — both insufficient for shared CI
            // runners). For backends that opt in via supports_split_archive,
            // probe a tiny `{base}.partcount` manifest published alongside
            // the parts: HTTP 200 means split, with the body giving N; 404
            // falls through to the single-file path. Non-split backends skip
            // this entirely so their install logs stay quiet.
            std::string base;
            if (is_tarball(filename)) {
                base = filename.substr(0, filename.size() - 7);  // strip ".tar.gz"
            }

            bool is_split = false;
            std::vector<std::string> part_assets;
            if (spec.supports_split_archive && is_tarball(filename)) {
                const std::string partcount_url = base_download_url + base + ".partcount";
                auto resp = utils::HttpClient::get(partcount_url);
                if (resp.status_code == 200) {
                    int total_parts = 0;
                    try {
                        std::string body = resp.body;
                        while (!body.empty()
                               && std::isspace(static_cast<unsigned char>(body.back()))) {
                            body.pop_back();
                        }
                        total_parts = std::stoi(body);
                    } catch (const std::exception& e) {
                        throw std::runtime_error(
                            "Malformed partcount at " + partcount_url + ": " + e.what());
                    }
                    if (total_parts < 1 || total_parts > 99) {
                        throw std::runtime_error(
                            "partcount out of range (1..99) at " + partcount_url
                            + ": got " + std::to_string(total_parts));
                    }
                    auto two_digit = [](int n) {
                        std::string s = std::to_string(n);
                        return s.size() < 2 ? std::string(2 - s.size(), '0') + s : s;
                    };
                    const std::string total_padded = two_digit(total_parts);
                    for (int i = 1; i <= total_parts; ++i) {
                        part_assets.push_back(base + ".part" + two_digit(i)
                                              + "-of-" + total_padded + ".tar.gz");
                    }
                    is_split = true;
                } else if (resp.status_code != 404) {
                    throw std::runtime_error(
                        "Unexpected HTTP " + std::to_string(resp.status_code)
                        + " when probing " + partcount_url);
                }
                // 404 = no manifest = single-file release; fall through.
            }
            if (!is_split) {
                std::string url = base_download_url + filename;
                LOG(DEBUG, spec.log_name()) << "Downloading from: " << url << std::endl;

                utils::ProgressCallback http_progress_cb;
                if (progress_cb) {
                    http_progress_cb = [&progress_cb, &filename](size_t downloaded, size_t total) -> bool {
                        DownloadProgress p;
                        p.file = filename;
                        p.file_index = 1;
                        p.total_files = 1;
                        p.bytes_downloaded = downloaded;
                        p.bytes_total = total;
                        p.percent = total > 0 ? static_cast<int>((downloaded * 100) / total) : 0;
                        p.complete = false;  // Don't signal complete until extraction is done
                        return progress_cb(p);
                    };
                } else {
                    http_progress_cb = utils::create_throttled_progress_callback();
                }

                utils::DownloadOptions archive_download_opts;
                archive_download_opts.expected_hash = lookup_expected_asset_hash(
                    spec.recipe, backend, expected_version, repo, filename);

                auto download_result = utils::HttpClient::download_file(
                    url, zip_path, http_progress_cb, {}, archive_download_opts);

                if (!download_result.success) {
                    throw std::runtime_error("Failed to download " + spec.binary + " from: " + url +
                                             " - " + download_result.error_message);
                }
            } else {
                // Split-archive path. Part names are known up front, but the
                // total byte size is not. Report per-part bytes and let the
                // caller/frontend aggregate by file_index / total_files.
                LOG(INFO, spec.log_name()) << "Downloading " << part_assets.size()
                                           << " split parts from " << repo << "@"
                                           << expected_version << std::endl;

                std::ofstream combined(zip_path, std::ios::binary);
                int part_index = 0;
                const int total_parts = static_cast<int>(part_assets.size());
                for (const auto& part_filename : part_assets) {
                    ++part_index;
                    std::string part_url = base_download_url + part_filename;
                    std::string part_path = zip_path + ".part" + std::to_string(part_index - 1);

                    LOG(DEBUG, spec.log_name()) << "Downloading part "
                                                << part_index << "/" << total_parts
                                                << ": " << part_filename << std::endl;

                    // Per-part progress wrapper. Keep byte fields scoped to
                    // the current part; do not synthesize total_download_size
                    // unless the true total across all parts is known.
                    utils::ProgressCallback part_http_cb;
                    if (progress_cb) {
                        int idx_snapshot = part_index;
                        std::string name_snapshot = part_filename;
                        part_http_cb = [&progress_cb, name_snapshot, idx_snapshot, total_parts]
                                      (size_t downloaded, size_t total) -> bool {
                            DownloadProgress p;
                            p.file = name_snapshot;
                            p.file_index = idx_snapshot;
                            p.total_files = total_parts;
                            p.bytes_downloaded = downloaded;
                            p.bytes_total = total;
                            p.percent = total > 0
                                ? static_cast<int>((downloaded * 100) / total)
                                : 0;
                            p.complete = false;
                            return progress_cb(p);
                        };
                    } else {
                        part_http_cb = utils::create_throttled_progress_callback();
                    }

                    utils::DownloadOptions part_download_opts;
                    part_download_opts.expected_hash = lookup_expected_asset_hash(
                        spec.recipe, backend, expected_version, repo, part_filename);

                    auto part_result = utils::HttpClient::download_file(
                        part_url, part_path, part_http_cb, {}, part_download_opts);

                    if (!part_result.success) {
                        combined.close();
                        fs::remove(part_path);
                        throw std::runtime_error("Failed to download " + part_filename + " from: " + part_url +
                                                 " - " + part_result.error_message);
                    }

                    // Append part to the combined archive
                    std::ifstream part_in(part_path, std::ios::binary);
                    combined << part_in.rdbuf();
                    part_in.close();
                    fs::remove(part_path);
                }
                combined.close();
            }

            LOG(DEBUG, spec.log_name()) << "Download complete!" << std::endl;

            // Verify the downloaded file
            if (!fs::exists(zip_path)) {
                throw std::runtime_error("Downloaded archive does not exist: " + zip_path);
            }

            std::uintmax_t file_size = fs::file_size(zip_path);
            LOG(DEBUG, spec.log_name()) << "Downloaded archive file size: "
                    << (file_size / 1024 / 1024) << " MB" << std::endl;

            // Extract into the staging directory (NOT install_dir) so a failed
            // extraction cannot destroy the currently-installed binary. The
            // staging guard removes the partial tree when we throw.
            if (!extract_archive(zip_path, staging_dir, spec.log_name())) {
                throw std::runtime_error("Failed to extract archive: " + zip_path);
            }

            // Save version info into the staging tree so it travels with the
            // atomic swap below. Fail cleanly on a write error rather than
            // promoting a backend with no version.txt (which would make the next
            // status check force an unnecessary reinstall).
            {
                const std::string staged_version_file = (fs::path(staging_dir) / "version.txt").string();
                std::ofstream vf(staged_version_file);
                vf << expected_version;
                vf.flush();
                if (!vf.good()) {
                    throw std::runtime_error("Failed to write version file: " + staged_version_file);
                }
            }

            // Normalize executable permissions for every regular file in the
            // staging tree.  Archives may place binaries under bin/ or directly
            // in the tree root (the llama.cpp Vulkan tarball does the latter),
            // and tarballs may strip the execute bit.  Recurse over the whole
            // tree so no layout is missed.  Fixing in staging (not post-swap)
            // preserves rollback on chmod failure.  On Windows chmod is a no-op.
            #ifndef _WIN32
            {
                for (const auto& entry : fs::recursive_directory_iterator(staging_dir)) {
                    if (entry.is_regular_file()) {
                        if (chmod(entry.path().c_str(), 0755) != 0) {
                            std::error_code ec;
                            ec.assign(errno, std::generic_category());
                            throw std::runtime_error(
                                "Failed to set executable permission on staged file "
                                + entry.path().string() + ": " + ec.message());
                        }
                    }
                }
            }
            #endif

            // Verify the staged tree contains the executable, then atomically
            // swap it into place. commit_staged_install keeps a recoverable .old
            // backup across the swap: it removes the staging tree and leaves
            // install_dir untouched on verification failure (returns ""), and on
            // a swap (rename) failure it rolls the backup back and throws — so a
            // botched download/extraction/swap never destroys the working binary.
            exe_path = commit_staged_install(staging_dir, install_dir, spec.binary);
            if (exe_path.empty()) {
                LOG(ERROR, spec.log_name()) << "Extraction completed but executable not found" << std::endl;
                throw std::runtime_error("Extraction failed: executable not found");
            }
            // Swap succeeded: staging was consumed by the rename, so disarm the guard.
            staging_guard.active = false;

            LOG(DEBUG, spec.log_name()) << "Executable verified at: " << exe_path << std::endl;

            // (The downloaded archive is removed by zip_guard on scope exit.)

            // Send completion event now that installation is fully done.
            // For split archives the combined on-disk size is only known after
            // all parts have been downloaded, so intermediate progress events
            // intentionally do not claim a total_download_size.
            if (progress_cb) {
                const int archive_total_files = is_split ? static_cast<int>(part_assets.size()) : 1;
                DownloadProgress p;
                p.file = filename;
                p.file_index = archive_total_files;
                p.total_files = archive_total_files;
                if (!is_split) {
                    p.bytes_downloaded = static_cast<size_t>(file_size);
                    p.bytes_total = static_cast<size_t>(file_size);
                    p.total_download_size = static_cast<size_t>(file_size);
                }
                p.percent = 100;
                p.complete = true;
                progress_cb(p);
            }

            LOG(DEBUG, spec.log_name()) << "Installation complete!" << std::endl;
        } else {
            LOG(DEBUG, spec.log_name()) << "Found executable at: " << exe_path << std::endl;

            // Even if already installed, send a completion event so callers know it's done
            if (progress_cb) {
                DownloadProgress p;
                p.file = asset_pattern;
                p.file_index = 1;
                p.total_files = 1;
                p.bytes_downloaded = 0;
                p.bytes_total = 0;
                p.percent = 100;
                p.complete = true;
                progress_cb(p);
            }
        }
    }
    namespace {
        // Non-throwing fs overloads so a bogus user-supplied path reports
        // "not a root" instead of throwing.
        std::optional<fs::path> validate_rocm_root(const fs::path& root) {
            std::error_code ec;
            if (root.empty() || !fs::exists(root, ec)) {
                return std::nullopt;
            }
#ifdef _WIN32
            // ROCm 5.x/6.x ship bin\amdhip64.dll; ROCm 7.x version-suffixes it
            // (bin\amdhip64_7.dll). Accept amdhip64.dll or amdhip64_<digits>.dll,
            // not arbitrary suffixes like amdhip64_backup.dll.
            const auto is_hip_runtime = [](const std::string& name) {
                if (name == "amdhip64.dll") {
                    return true;
                }
                static const std::string prefix = "amdhip64_";
                static const std::string suffix = ".dll";
                if (name.size() <= prefix.size() + suffix.size() ||
                    name.compare(0, prefix.size(), prefix) != 0 ||
                    name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
                    return false;
                }
                const auto digits = name.substr(
                    prefix.size(), name.size() - prefix.size() - suffix.size());
                return std::all_of(digits.begin(), digits.end(),
                                   [](unsigned char c) { return std::isdigit(c); });
            };
            for (const char* subdir : {"bin", "lib"}) {
                const fs::path dir = root / subdir;
                if (!fs::is_directory(dir, ec)) {
                    continue;
                }
                for (fs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
                    if (it->is_regular_file(ec) &&
                        is_hip_runtime(it->path().filename().string())) {
                        return root;
                    }
                }
            }
#else
            for (const char* lib_subdir : {"lib", "lib64"}) {
                if (fs::exists(root / lib_subdir / "libamdhip64.so", ec)) {
                    return root;
                }
            }
#endif
            return std::nullopt;
        }

        std::optional<fs::path> query_rocm_sdk_root() {
#ifdef _WIN32
            // SearchPathA (used by find_executable_in_path) does not append a
            // default extension, so the console-script shim must be named
            // explicitly. CreateProcess resolves the .exe for the spawn itself.
            const char* rocm_sdk_exe = "rocm-sdk.exe";
#else
            const char* rocm_sdk_exe = "rocm-sdk";
#endif
            if (utils::find_executable_in_path(rocm_sdk_exe).empty()) {
                return std::nullopt;
            }

            // run_process_with_output merges the child's stderr into stdout, and
            // rocm-sdk is a Python console script that may emit warnings there.
            // Collect every line and pick the first that validates, rather than
            // trusting the first line (which could be a warning).
            std::vector<std::string> lines;
            auto on_line = [&lines](const std::string& line) {
                lines.push_back(line);
                return true;
            };

            int rc = utils::ProcessManager::run_process_with_output(
                "rocm-sdk", {"path", "--root"}, on_line, /*working_dir=*/"",
                /*timeout_seconds=*/5);
            if (rc != 0) {
                LOG(DEBUG, "BackendUtils") << "rocm-sdk path --root exited with " << rc
                          << "; ignoring" << std::endl;
                return std::nullopt;
            }

            for (const auto& candidate : BackendUtils::pick_rocm_root_candidates(lines)) {
                if (auto root = validate_rocm_root(fs::path(candidate))) {
                    return root;
                }
            }
            return std::nullopt;
        }
    }  // namespace

    std::optional<fs::path> BackendUtils::resolve_rocm_root(bool* resolved_explicitly) {
        if (resolved_explicitly) {
            *resolved_explicitly = false;
        }

        if (const char* env = std::getenv("ROCM_PATH"); env && *env != '\0') {
            if (auto root = validate_rocm_root(fs::path(env))) {
                if (resolved_explicitly) {
                    *resolved_explicitly = true;
                }
                LOG(DEBUG, "BackendUtils") << "Resolved ROCm root from ROCM_PATH: "
                          << root->string() << std::endl;
                return root;
            }
            LOG(DEBUG, "BackendUtils") << "ROCM_PATH=" << env
                      << " has no HIP runtime; trying other sources" << std::endl;
        }

        if (auto sdk_root = query_rocm_sdk_root()) {
            if (resolved_explicitly) {
                *resolved_explicitly = true;
            }
            LOG(DEBUG, "BackendUtils") << "Resolved ROCm root from rocm-sdk: "
                      << sdk_root->string() << std::endl;
            return *sdk_root;
        }

#ifdef _WIN32
        // The AMD HIP SDK installer sets HIP_PATH; treat it as the platform
        // default (like /opt/rocm on Linux), not a user selection.
        if (const char* hip = std::getenv("HIP_PATH"); hip && *hip != '\0') {
            if (auto root = validate_rocm_root(fs::path(hip))) {
                LOG(DEBUG, "BackendUtils") << "Resolved ROCm root at default HIP_PATH: "
                          << root->string() << std::endl;
                return root;
            }
        }
#else
        if (auto root = validate_rocm_root("/opt/rocm")) {
            LOG(DEBUG, "BackendUtils") << "Resolved ROCm root at default /opt/rocm" << std::endl;
            return root;
        }
#endif

        return std::nullopt;
    }

    std::vector<std::string> BackendUtils::pick_rocm_root_candidates(
        const std::vector<std::string>& lines) {
        std::vector<std::string> candidates;
        for (const auto& line : lines) {
            const auto first = line.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) {
                continue;
            }
            const auto last = line.find_last_not_of(" \t\r\n");
            std::string trimmed = line.substr(first, last - first + 1);
            if (fs::path(trimmed).is_absolute()) {
                candidates.push_back(std::move(trimmed));
            }
        }
        return candidates;
    }

    std::string BackendUtils::read_rocm_version_from_root(const fs::path& root) {
        const std::vector<fs::path> version_paths = {
            root / ".info" / "version",
            root / "share" / "rocm" / "version",
            root / "version"
        };
        for (const auto& version_path : version_paths) {
            std::error_code ec;
            if (!fs::exists(version_path, ec)) {
                continue;
            }
            std::ifstream file(version_path);
            if (!file.is_open()) {
                continue;
            }
            std::string line;
            std::getline(file, line);
            const auto first = line.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) {
                continue;
            }
            const auto last = line.find_last_not_of(" \t\r\n");
            return line.substr(first, last - first + 1);
        }
        return "";
    }

    std::string BackendUtils::get_therock_install_dir(const std::string& arch, const std::string& version) {
        fs::path therock_base = fs::path(utils::get_downloaded_bin_dir()) / "therock";
        return (therock_base / (arch + "-" + version)).string();
    }

    void BackendUtils::cleanup_old_therock_versions(const std::string& current_version) {
#ifdef __linux__
        fs::path therock_base = fs::path(utils::get_downloaded_bin_dir()) / "therock";

        if (!fs::exists(therock_base)) {
            return;
        }

        try {
            for (const auto& entry : fs::directory_iterator(therock_base)) {
                if (entry.is_directory()) {
                    std::string dir_name = entry.path().filename().string();
                    size_t dash_pos = dir_name.rfind('-');
                    if (dash_pos != std::string::npos) {
                        std::string version = dir_name.substr(dash_pos + 1);
                        if (version != current_version) {
                            LOG(DEBUG, "BackendUtils") << "Cleaning up old TheRock version: " << dir_name << std::endl;
                            fs::remove_all(entry.path());
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            LOG(WARNING, "BackendUtils") << "Failed to cleanup old TheRock versions: " << e.what() << std::endl;
        }
#endif
    }

    void BackendUtils::install_therock(const std::string& arch, const std::string& version,
                                       DownloadProgressCallback progress_cb) {
#if !defined(__linux__) && !defined(_WIN32)
        throw std::runtime_error("TheRock is only supported on Linux and Windows");
#else
        std::string install_dir = get_therock_install_dir(arch, version);
        std::string version_file = (fs::path(install_dir) / "version.txt").string();

        if (fs::exists(install_dir) && fs::exists(version_file)) {
            std::string installed_version;
            std::ifstream vf(version_file);
            std::getline(vf, installed_version);
            vf.close();

            if (installed_version == version) {
                LOG(DEBUG, "BackendUtils") << "TheRock " << arch << "-" << version << " already installed" << std::endl;
                return;
            }
        }

        LOG(INFO, "BackendUtils") << "Installing TheRock ROCm " << version << " for " << arch << std::endl;

        fs::create_directories(install_dir);

        std::string config_path = utils::get_resource_path("resources/backend_versions.json");
        json config = utils::JsonUtils::load_from_file(config_path);

        std::string url_variant = arch;
        if (config.contains("therock") && config["therock"].contains("url_mapping") &&
            config["therock"]["url_mapping"].contains(arch)) {
            url_variant = config["therock"]["url_mapping"][arch].get<std::string>();
        }

#ifdef _WIN32
        std::string platform = "windows";
#else
        std::string platform = "linux";
#endif
        std::string filename = "therock-dist-" + platform + "-" + url_variant + "-" + version + ".tar.gz";
        std::string url = "https://repo.amd.com/rocm/tarball/" + filename;

        fs::path cache_dir = get_backend_download_cache_dir();
        std::string tarball_path = (cache_dir / filename).string();

        LOG(DEBUG, "BackendUtils") << "Downloading TheRock from: " << url << std::endl;
        LOG(DEBUG, "BackendUtils") << "Downloading to: " << tarball_path << std::endl;

        // Create progress callback for download
        utils::ProgressCallback http_progress_cb;
        if (progress_cb) {
            http_progress_cb = [&progress_cb, &filename](size_t downloaded, size_t total) -> bool {
                DownloadProgress p;
                p.file = filename;
                p.file_index = 1;
                p.total_files = 1;
                p.bytes_downloaded = downloaded;
                p.bytes_total = total;
                p.percent = total > 0 ? static_cast<int>((downloaded * 100) / total) : 0;
                p.complete = false;
                return progress_cb(p);
            };
        } else {
            http_progress_cb = utils::create_throttled_progress_callback();
        }

        // TheRock asset verification stays metadata-driven. Once upstream publishes
        // per-asset checksums, backend_versions.json can provide them without
        // changing download code. Until then this remains an optional lookup.
        utils::DownloadOptions therock_download_opts;
        therock_download_opts.expected_hash = lookup_expected_asset_hash(
            "therock", arch, version, "", filename);

        auto download_result = utils::HttpClient::download_file(
            url,
            tarball_path,
            http_progress_cb,
            {},
            therock_download_opts
        );

        if (!download_result.success) {
            throw std::runtime_error("Failed to download TheRock from: " + url +
                                    " - " + download_result.error_message);
        }

        LOG(DEBUG, "BackendUtils") << "TheRock download complete" << std::endl;

        if (!fs::exists(tarball_path)) {
            throw std::runtime_error("Downloaded TheRock tarball does not exist: " + tarball_path);
        }

        std::uintmax_t file_size = fs::file_size(tarball_path);
        LOG(DEBUG, "BackendUtils") << "Downloaded tarball size: "
                                    << (file_size / 1024 / 1024) << " MB" << std::endl;

        if (!extract_tarball(tarball_path, install_dir, "TheRock")) {
            fs::remove(tarball_path);
            fs::remove_all(install_dir);
            throw std::runtime_error("Failed to extract TheRock tarball: " + tarball_path);
        }

#ifdef _WIN32
        // On Windows, DLLs are in bin/ (lib/ contains only import .lib files)
        fs::path runtime_dir = fs::path(install_dir) / "bin";
        if (!fs::exists(runtime_dir)) {
            fs::remove(tarball_path);
            fs::remove_all(install_dir);
            throw std::runtime_error("TheRock extraction failed: bin directory not found");
        }
        LOG(DEBUG, "BackendUtils") << "TheRock bin directory verified at: " << runtime_dir << std::endl;
#else
        // On Linux, shared libraries are in lib/
        fs::path runtime_dir = fs::path(install_dir) / "lib";
        if (!fs::exists(runtime_dir)) {
            fs::remove(tarball_path);
            fs::remove_all(install_dir);
            throw std::runtime_error("TheRock extraction failed: lib directory not found");
        }
        LOG(DEBUG, "BackendUtils") << "TheRock lib directory verified at: " << runtime_dir << std::endl;
#endif

        std::ofstream vf(version_file);
        vf << version;
        vf.close();

        fs::remove(tarball_path);
        cleanup_old_therock_versions(version);

        // Send completion notification
        if (progress_cb) {
            DownloadProgress p;
            p.file = filename;
            p.file_index = 1;
            p.total_files = 1;
            p.bytes_downloaded = download_result.bytes_downloaded;
            p.bytes_total = download_result.total_bytes;
            p.percent = 100;
            p.complete = true;
            progress_cb(p);
        }

        LOG(INFO, "BackendUtils") << "TheRock installation complete" << std::endl;
#endif
    }

    std::string BackendUtils::get_therock_lib_path(const std::string& rocm_arch) {
#if !defined(__linux__) && !defined(_WIN32)
        return "";
#else
        std::string config_path = utils::get_resource_path("resources/backend_versions.json");
        json config = utils::JsonUtils::load_from_file(config_path);

        if (!config.contains("therock") || !config["therock"].contains("version")) {
            throw std::runtime_error("backend_versions.json is missing 'therock.version'");
        }

        std::string version = config["therock"]["version"].get<std::string>();

        // Only return the path if TheRock is already installed
        std::string install_dir = get_therock_install_dir(rocm_arch, version);
        if (fs::exists(install_dir)) {
#ifdef _WIN32
            // On Windows, DLLs are in bin/ (lib/ contains only import .lib files)
            std::string lib_path = (fs::path(install_dir) / "bin").string();
#else
            // On Linux, shared libraries are in lib/
            std::string lib_path = (fs::path(install_dir) / "lib").string();
#endif
            LOG(DEBUG, "BackendUtils") << "Returning TheRock runtime path: " << lib_path << std::endl;
            return lib_path;
        }

        return "";
#endif
    }
    void BackendUtils::apply_cuda_env_vars(
            std::vector<std::pair<std::string, std::string>>& env_vars,
            const std::string& log_tag,
            bool skip_visible_devices) {
        if (!skip_visible_devices) {
            const char* existing_visible_devices = std::getenv("CUDA_VISIBLE_DEVICES");
            const bool has_visible_override = existing_visible_devices && existing_visible_devices[0] != '\0';

            if (has_visible_override) {
                LOG(INFO, log_tag) << "Respecting existing CUDA_VISIBLE_DEVICES="
                                   << existing_visible_devices << std::endl;
            } else {
                std::string cuda_arch = SystemInfo::get_cuda_arch();
                std::string visible_devices = SystemInfo::get_cuda_visible_devices_for_arch(cuda_arch);
                if (!cuda_arch.empty() && !visible_devices.empty()) {
                    env_vars.push_back({"CUDA_VISIBLE_DEVICES", visible_devices});
                    LOG(INFO, log_tag)
                        << "Restricting CUDA_VISIBLE_DEVICES to " << visible_devices
                        << " for " << cuda_arch
                        << " CUDA asset; matching same-arch GPUs remain available for multi-GPU offload"
                        << std::endl;
                }
            }
        }

#ifdef __linux__
        const char* existing_prime = std::getenv("__NV_PRIME_RENDER_OFFLOAD");
        if (!existing_prime || existing_prime[0] == '\0') {
            env_vars.push_back({"__NV_PRIME_RENDER_OFFLOAD", "1"});
            LOG(INFO, log_tag) << "Setting __NV_PRIME_RENDER_OFFLOAD=1 for PRIME Offload compatibility"
                               << std::endl;
        }
#endif
    }
} // namespace lemon::backends

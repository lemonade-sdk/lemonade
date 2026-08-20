#include "lemon/backends/backend_utils.h"
#include "lemon/backends/install_staging.h"
#include "lemon/runtime_config.h"
#include "lemon/system_info.h"
#include "lemon/backends/backend_registry.h"  // spec_for() — descriptor->install spec, no server includes
#include "lemon/model_manager.h"  // For DownloadProgress, DownloadProgressCallback

#include "lemon/utils/github_api.h"
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
#include <string>
#include <lemon/utils/aixlog.hpp>
#include <algorithm>
#include <system_error>
#include <vector>
#include <nlohmann/json.hpp>

#ifdef _WIN32
    #include <windows.h>
    #include <winver.h>
    #pragma comment(lib, "version.lib")
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

        LOG(DEBUG, spec.log_name()) << "Resolving asset wildcard '" << pattern
            << "' for " << repo << "@" << tag << " via " << url << std::endl;

        utils::HttpResponse resp;
        try {
            resp = utils::github_api::get(url);
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
        std::string section, bin_key;
        build_bin_config_key(recipe, backend, section, bin_key);
        std::string bin_value = get_bin_config_value(recipe, backend);

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
        std::string section, bin_key;
        build_bin_config_key(recipe, backend, section, bin_key);

        std::string env_name = "LEMONADE_" + section + "_" + bin_key;
        std::transform(env_name.begin(), env_name.end(), env_name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

        std::string bin_value = utils::get_environment_variable_utf8(env_name);
        if (!bin_value.empty()) return bin_value;

        auto* cfg = lemon::RuntimeConfig::global();
        if (!cfg) return "";
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
                archive_download_opts.resume_partial = false;

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
                    part_download_opts.resume_partial = false;

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
        // Track whether the DownloadProgressCallback returned false (user
        // cancellation) during a wheel installation. This prevents
        // install_rocm_runtime from falling back to the tarball after a
        // user-initiated cancel.
        thread_local bool g_therock_wheels_cancelled = false;

        void reset_therock_wheels_cancelled() {
            g_therock_wheels_cancelled = false;
        }

        bool get_therock_wheels_cancelled() {
            return g_therock_wheels_cancelled;
        }

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

    std::string BackendUtils::get_therock_wheel_dir(const std::string& arch, const std::string& version) {
        fs::path base = fs::path(utils::get_downloaded_bin_dir()) / "therock-wheels";
        return (base / (arch + "-" + version)).string();
    }

    namespace {
        // backend_versions.json is a build resource. Parse once so the
        // thermrock install path can look up the URL mapping. Thread-safe:
        // C++11 guarantees a single, synchronized initialization of a
        // function-local static.
        const json& backend_versions_config() {
            static const json config = utils::JsonUtils::load_from_file(
                utils::get_resource_path("resources/backend_versions.json"));
            return config;
        }
    }  // namespace

    namespace {
        // Locate a Python interpreter that can create virtual environments.
        std::string find_python_for_venv() {
#ifdef _WIN32
            const std::vector<std::string> names = {"python.exe", "python3.exe"};
#else
            const std::vector<std::string> names = {"python3", "python"};
#endif
            for (const auto& name : names) {
                std::string path = utils::find_executable_in_path(name);
                if (path.empty()) {
                    continue;
                }
#ifdef _WIN32
                // Skip the Microsoft Store alias: it pops the Store UI and stalls
                // the probe ~30 s instead of running Python.
                std::string lower = path;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower.find("windowsapps") != std::string::npos) {
                    continue;
                }
#endif
                // Confirm venv can actually build a working environment. On
                // Debian/Ubuntu `import venv` succeeds without python3-venv, but
                // `python -m venv` then fails on the missing ensurepip; probing
                // both up front makes the tarball fallback immediate.
                int rc = utils::ProcessManager::run_process_with_output(
                    path, {"-c", "import venv, ensurepip"}, nullptr, /*working_dir=*/"",
                    /*timeout_seconds=*/30);
                if (rc == 0) {
                    return path;
                }
            }
            return "";
        }

        // Ask the managed venv's Python where the ROCm runtime libraries landed.
        // The rocm-sdk-core/-libraries wheels expose them under
        // _rocm_sdk_core/{bin,lib} and _rocm_sdk_libraries/{bin,lib}; we print
        // every candidate and keep the directories that actually exist so the
        // logic is correct on both Windows (bin) and Linux (lib).
        std::vector<std::string> query_wheel_runtime_dirs(const std::string& venv_python) {
            // Print candidate {bin,lib,llvm} dirs for each rocm-sdk runtime package.
            // Keep it tolerant: a missing package or a namespace package (whose
            // __file__ is None in kpack-split mode) must not abort the probe, so
            // each import is guarded and __path__ is used as a fallback root.
            static const char* probe =
#ifdef _WIN32
                "import importlib,os\n"
                "for m in ('_rocm_sdk_core','_rocm_sdk_libraries'):\n"
                "    try:\n"
                "        mod=importlib.import_module(m)\n"
                "    except Exception:\n"
                "        continue\n"
                "    root=os.path.dirname(mod.__file__) if getattr(mod,'__file__',None) "
                "else (list(getattr(mod,'__path__',[]))[:1] or [''])[0]\n"
                "    if not root:\n"
                "        continue\n"
                "    for s in ('bin','lib',os.path.join('lib','llvm','bin')):\n"
                "        print(os.path.join(root,s))\n";
#else
                "import importlib,os\n"
                "for m in ('_rocm_sdk_core','_rocm_sdk_libraries'):\n"
                "    try:\n"
                "        mod=importlib.import_module(m)\n"
                "    except Exception:\n"
                "        continue\n"
                "    root=os.path.dirname(mod.__file__) if getattr(mod,'__file__',None) "
                "else (list(getattr(mod,'__path__',[]))[:1] or [''])[0]\n"
                "    if not root:\n"
                "        continue\n"
                "    for s in ('lib',os.path.join('lib','llvm','lib')):\n"
                "        print(os.path.join(root,s))\n";
#endif

            std::vector<std::string> lines;
            auto on_line = [&lines](const std::string& line) {
                lines.push_back(line);
                return true;
            };
            // -X utf8 so non-ASCII paths print as UTF-8 regardless of host locale.
            int rc = utils::ProcessManager::run_process_with_output(
                venv_python, {"-X", "utf8", "-c", probe}, on_line, /*working_dir=*/"",
                /*timeout_seconds=*/60);
            if (rc != 0) {
                return {};
            }

            std::vector<std::string> dirs;
            for (auto& line : lines) {
                std::string trimmed = line;
                while (!trimmed.empty() &&
                       (trimmed.back() == '\r' || trimmed.back() == '\n' ||
                        trimmed.back() == ' ' || trimmed.back() == '\t')) {
                    trimmed.pop_back();
                }
                std::error_code ec;
                if (!trimmed.empty() && fs::is_directory(utils::path_from_utf8(trimmed), ec)) {
                    dirs.push_back(trimmed);
                }
            }
            return dirs;
        }

        void cleanup_stale_version_dirs(const fs::path& base,
                                        const std::string& keep) {
            std::error_code ec;
            if (!fs::exists(base, ec)) {
                return;
            }
            for (fs::directory_iterator it(base, ec), end; it != end && !ec; it.increment(ec)) {
                if (!it->is_directory(ec)) {
                    continue;
                }
                if (it->path().filename().string() != keep) {
                    LOG(DEBUG, "BackendUtils")
                        << "Cleaning up old ROCm install: " << it->path().filename().string() << std::endl;
                    fs::remove_all(it->path(), ec);
                }
            }
        }
    }  // namespace

    void BackendUtils::cleanup_old_therock_versions() {
        const json& config = backend_versions_config();

        if (!config.contains("therock") || !config["therock"].is_object() ||
            !config["therock"].contains("version") || !config["therock"]["version"].is_string()) {
            return;
        }

        std::string version = config["therock"]["version"].get<std::string>();
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
                        std::string dir_version = dir_name.substr(dash_pos + 1);
                        if (dir_version != version) {
                            LOG(DEBUG, "BackendUtils") << "Cleaning up old TheRock version: " << dir_name << std::endl;
                            fs::remove_all(entry.path());
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            LOG(WARNING, "BackendUtils")
                << "Failed to cleanup old TheRock versions: " << e.what() << std::endl;
        }
#else
        (void)version;
#endif
    }

    void BackendUtils::install_rocm_runtime(const std::string& arch, const std::string& version,
                                            DownloadProgressCallback progress_cb) {
        // rocm_install_method lets Python-averse environments (ISV/OEM images,
        // air-gapped or pip-restricted hosts) opt out of the wheel path:
        //   auto    - pip wheels, TheRock tarball fallback (default)
        //   wheel   - pip wheels only (no tarball)
        //   tarball - TheRock tarball only (no Python/pip)
        // method.txt records "wheel" or "tarball" at install time; when methods
        // disagree (e.g. user switched knobs), backend_manager triggers reinstall.
        std::string method = "auto";
        if (auto* cfg = RuntimeConfig::global()) {
            method = cfg->rocm_install_method();
        }

        reset_therock_wheels_cancelled();

        if (method != "tarball") {
            if (install_therock_wheels(arch, version, progress_cb)) {
                // Drop the other tree so its method.txt can't re-trigger the
                // mismatch reinstall on every load.
                if (method == "wheel") {
                    std::error_code ec;
                    fs::remove_all(get_therock_install_dir(arch, version), ec);
                }
                return;
            }
            if (get_therock_wheels_cancelled()) {
                // Throw rather than return: a silent return lets the caller
                // report the runtime step as complete ("successfully installed")
                // and would also fall through to the tarball the user cancelled.
                LOG(INFO, "BackendUtils") << "ROCm install cancelled by user" << std::endl;
                throw std::runtime_error("ROCm runtime installation cancelled");
            }
            if (method == "wheel") {
                throw std::runtime_error(
                    "ROCm wheel install failed and rocm_install_method=wheel "
                    "disables the TheRock tarball fallback");
            }
        }
        install_therock(arch, version, progress_cb);
        // Drop the other tree so its method.txt can't re-trigger the mismatch
        // reinstall on every load.
        if (method == "tarball") {
            std::error_code ec;
            fs::remove_all(get_therock_wheel_dir(arch, version), ec);
        }
    }

    bool BackendUtils::is_concrete_gfx_arch(const std::string& arch) {
        if (arch.rfind("gfx", 0) != 0 || arch.size() <= 3) {
            return false;
        }
        for (size_t i = 3; i < arch.size(); ++i) {
            if (!std::isxdigit(static_cast<unsigned char>(arch[i]))) {
                return false;
            }
        }
        return true;
    }

    bool BackendUtils::parse_pip_download_line(const std::string& line,
                                               std::string& filename,
                                               size_t& bytes_total) {
        filename.clear();
        bytes_total = 0;

        static const std::string dl_marker = "Downloading ";
        const size_t dl_pos = line.find(dl_marker);
        if (dl_pos == std::string::npos) {
            return false;
        }

        const std::string rest = line.substr(dl_pos + dl_marker.size());
        // Filename is the first whitespace-delimited token.
        const size_t sp = rest.find_first_of(" \t");
        const std::string token = sp == std::string::npos ? rest : rest.substr(0, sp);
        const size_t slash = token.find_last_of("/\\");
        filename = slash == std::string::npos ? token : token.substr(slash + 1);

        // Search the full remainder, not the whitespace-cut token: pip prints
        // "Downloading <name> (414.0 MB)", so the parens follow the filename.
        const size_t lparen = rest.rfind('(');
        const size_t rparen = rest.rfind(')');
        if (lparen == std::string::npos || rparen == std::string::npos || rparen <= lparen + 1) {
            return true;
        }

        const std::string size_part = rest.substr(lparen + 1, rparen - lparen - 1);
        // pip prints units uppercase (kB/MB/GB), hence the case-insensitive match.
        double multiplier = 1.0;
        size_t unit_pos = std::string::npos;
        for (size_t i = 0; i < size_part.size(); ++i) {
            const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(size_part[i])));
            if (c == 'k') { multiplier = 1024.0; unit_pos = i; break; }
            if (c == 'm') { multiplier = 1048576.0; unit_pos = i; break; }
            if (c == 'g') { multiplier = 1073741824.0; unit_pos = i; break; }
        }

        std::string num_str = unit_pos == std::string::npos ? size_part : size_part.substr(0, unit_pos);
        while (!num_str.empty() && (num_str.back() == ' ' || num_str.back() == '\t')) {
            num_str.pop_back();
        }
        try {
            bytes_total = static_cast<size_t>(std::stod(num_str) * multiplier);
        } catch (const std::exception&) {
            bytes_total = 0;
        }
        return true;
    }

    bool BackendUtils::install_therock_wheels(const std::string& arch, const std::string& version,
                                              DownloadProgressCallback progress_cb) {
#if !defined(__linux__) && !defined(_WIN32)
        (void)arch; (void)version; (void)progress_cb;
        return false;
#else
        if (!is_concrete_gfx_arch(arch)) {
            LOG(DEBUG, "BackendUtils")
                << "No rocm-sdk device wheel for '" << arch
                << "'; using TheRock tarball" << std::endl;
            return false;
        }

        std::string python = find_python_for_venv();
        if (python.empty()) {
            LOG(INFO, "BackendUtils")
                << "Python with venv support not found; using TheRock tarball" << std::endl;
            return false;
        }

        const std::string wheel_dir = get_therock_wheel_dir(arch, version);
        const fs::path venv_dir = fs::path(wheel_dir) / "venv";
        const fs::path paths_file = fs::path(wheel_dir) / "runtime_paths.txt";
        const fs::path version_file = fs::path(wheel_dir) / "version.txt";

        // Idempotent: skip when the same version is already installed AND its
        // recorded runtime dirs still exist. A version marker whose runtime was
        // deleted or moved (stale absolute paths) must fall through to a fresh
        // reinstall below, not short-circuit into a dead loader path.
        if (fs::exists(version_file) && fs::exists(paths_file)) {
            std::ifstream vf(version_file);
            std::string installed;
            std::getline(vf, installed);
            if (installed == version && therock_wheel_runtime_alive(arch, version)) {
                LOG(DEBUG, "BackendUtils")
                    << "ROCm wheels " << arch << "-" << version
                    << " already installed" << std::endl;
                return true;
            }
        }

        LOG(INFO, "BackendUtils") << "Installing ROCm " << version
            << " via pip wheels for " << arch << " (this may take several minutes)" << std::endl;

        std::error_code ec;
        fs::remove_all(wheel_dir, ec);
        fs::create_directories(wheel_dir, ec);

        int downloads_seen = 0;
        size_t bytes_total = 0;
        size_t bytes_downloaded = 0;
        bool install_phase_started = false;

        // Emit an initial 0% event so the download manager identifies this
        // row immediately (otherwise it sits with no name while venv + pip
        // resolve phases take several seconds with zero output).
        if (progress_cb) {
            DownloadProgress p;
            p.file = "ROCm runtime (preparing venv)";
            p.file_index = 0;
            p.total_files = 2;
            p.percent = 0;
            p.complete = false;
            progress_cb(p);
        }

        auto log_line = [&progress_cb, &downloads_seen,
                         &bytes_total, &bytes_downloaded, &install_phase_started](const std::string& line) {
            LOG(INFO, "BackendUtils") << "(pip) " << line << std::endl;
            if (!progress_cb) {
                return true;
            }

            // pip (non-interactive) prints one "Downloading <name> (<size>)" line
            // per wheel. Surface it so the UI shows movement across the ~730 MB
            // install, parse the size for a real progress bar, and honour the
            // callback's cancellation return by killing pip.
            std::string fname;
            size_t this_size = 0;
            if (parse_pip_download_line(line, fname, this_size)) {
                bytes_total += this_size;
                DownloadProgress p;
                p.file = fname;
                p.bytes_total = bytes_total;
                p.bytes_downloaded = bytes_downloaded;
                p.file_index = ++downloads_seen;
                p.total_files = 2;
                p.complete = false;
                p.percent = bytes_total > 0
                    ? static_cast<int>((bytes_downloaded * 100) / bytes_total)
                    : 0;
                bool keep = progress_cb(p);
                if (!keep) {
                    g_therock_wheels_cancelled = true;
                }
                bytes_downloaded += this_size;
                // Emit updated byte progress for this wheel. This is NOT a
                // completion event: pip downloads many wheels and the whole
                // runtime is only done once install_therock_wheels succeeds, so
                // complete stays false here to keep the download manager from
                // reporting the entire install finished after the first wheel.
                if (keep) {
                    DownloadProgress done;
                    done.file = fname;
                    done.file_index = downloads_seen;
                    done.total_files = 2;
                    done.bytes_total = bytes_total;
                    done.bytes_downloaded = bytes_downloaded;
                    done.percent = bytes_total > 0
                        ? static_cast<int>((bytes_downloaded * 100) / bytes_total)
                        : 0;
                    done.complete = false;
                    progress_cb(done);
                }
                return keep;
            }

            // Detect pip's install phase; emit a coarse progress bump so the
            // long unpack / hash / link phase doesn't stall at a static value.
            static const std::string install_markers[] = {
                "Installing collected packages",
                "Processing ",
            };
            for (const auto& marker : install_markers) {
                if (line.find(marker) != std::string::npos) {
                    install_phase_started = true;
                    DownloadProgress p;
                    p.file = "ROCm runtime (installing)";
                    p.file_index = downloads_seen > 0 ? downloads_seen : 1;
                    p.total_files = 2;
                    p.bytes_total = bytes_total;
                    p.bytes_downloaded = bytes_downloaded;
                    p.percent = bytes_total > 0
                        ? static_cast<int>((bytes_downloaded * 100) / bytes_total)
                        : 95;
                    p.complete = false;
                    bool keep = progress_cb(p);
                    if (!keep) {
                        g_therock_wheels_cancelled = true;
                    }
                    return keep;
                }
            }

            return true;
        };

        int rc = utils::ProcessManager::run_process_with_output(
            python, {"-m", "venv", utils::path_to_utf8(venv_dir)}, log_line,
            /*working_dir=*/"", /*timeout_seconds=*/300);
        if (rc != 0) {
            LOG(WARNING, "BackendUtils")
                << "Failed to create venv (exit " << rc
                << "); using TheRock tarball" << std::endl;
            fs::remove_all(wheel_dir, ec);
            return false;
        }

#ifdef _WIN32
        const std::string venv_python =
            utils::path_to_utf8(venv_dir / "Scripts" / "python.exe");
#else
        const std::string venv_python =
            utils::path_to_utf8(venv_dir / "bin" / "python");
#endif

        const std::string index_url = "https://repo.amd.com/rocm/whl-multi-arch/";
        const std::string spec =
            "rocm[libraries,device-" + arch + "]==" + version;

        rc = utils::ProcessManager::run_process_with_output(
            venv_python,
            // --isolated ignores user/host pip config and PIP_EXTRA_INDEX_URL so a
            // machine-level extra index can't shadow rocm-sdk-*. Note: `rocm` is an
            // sdist, so pip build-isolates it and fetches setuptools from this same
            // index — an implicit dependency on AMD's index contents.
            // --no-cache-dir: the wheels land in the venv, so a ~730 MB pip cache
            // that nothing reuses only wastes disk.
            {"-m", "pip", "install", "--isolated", "--no-cache-dir", "--no-input",
             "--index-url", index_url, spec},
            log_line, /*working_dir=*/"", /*timeout_seconds=*/1800);
        if (rc != 0) {
            LOG(WARNING, "BackendUtils")
                << "pip install of ROCm wheels failed (exit " << rc
                << "); using TheRock tarball" << std::endl;
            fs::remove_all(wheel_dir, ec);
            return false;
        }

        // pip treats an unknown "device-<arch>" extra as a warning and still exits
        // 0, installing rocm-sdk-libraries (which provides libamdhip64) but NO
        // device code objects. Confirm the device package actually resolved;
        // otherwise fall back to the tarball rather than caching a runtime that
        // can't run kernels for this GPU.
        {
            const std::string device_pkg = "rocm-sdk-device-" + arch;
            int show_rc = utils::ProcessManager::run_process_with_output(
                venv_python, {"-m", "pip", "show", device_pkg}, nullptr,
                /*working_dir=*/"", /*timeout_seconds=*/60);
            if (show_rc != 0) {
                LOG(WARNING, "BackendUtils")
                    << "ROCm device wheel '" << device_pkg << "' did not resolve (no "
                    << "code objects for " << arch << "); using TheRock tarball" << std::endl;
                fs::remove_all(wheel_dir, ec);
                return false;
            }
        }

        std::vector<std::string> runtime_dirs = query_wheel_runtime_dirs(venv_python);
        bool has_hip_runtime = false;
        for (const auto& dir : runtime_dirs) {
            for (fs::directory_iterator it(utils::path_from_utf8(dir), ec), end;
                 it != end && !ec; it.increment(ec)) {
                const std::string name = it->path().filename().string();
#ifdef _WIN32
                if (name.rfind("amdhip64", 0) == 0) {
#else
                if (name.rfind("libamdhip64.so", 0) == 0) {
#endif
                    has_hip_runtime = true;
                    break;
                }
            }
            if (has_hip_runtime) {
                break;
            }
        }
        if (runtime_dirs.empty() || !has_hip_runtime) {
            LOG(WARNING, "BackendUtils")
                << "ROCm wheels installed but the HIP runtime could not be located; "
                << "using TheRock tarball" << std::endl;
            fs::remove_all(wheel_dir, ec);
            return false;
        }

        {
            std::ofstream pf(paths_file);
            for (const auto& dir : runtime_dirs) {
                pf << dir << "\n";
            }
        }
        {
            std::ofstream mf(fs::path(wheel_dir) / "method.txt");
            mf << "wheel";
        }
        // version.txt is written last: both the idempotency check above and
        // is_therock_installed_for_current_arch key off it, so once it exists the
        // other markers (runtime_paths.txt, method.txt) are guaranteed present.
        {
            std::ofstream vf(version_file);
            vf << version;
        }

        // Drop other wheel versions for this base dir to bound disk usage.
        cleanup_stale_version_dirs(
            fs::path(utils::get_downloaded_bin_dir()) / "therock-wheels",
            fs::path(wheel_dir).filename().string());

        if (progress_cb) {
            DownloadProgress p;
            p.file = "ROCm runtime";
            p.file_index = 1;
            p.total_files = 1;
            p.percent = 100;
            p.complete = true;
            progress_cb(p);
        }

        LOG(INFO, "BackendUtils") << "ROCm wheel installation complete" << std::endl;
        return true;
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

        const json& config = backend_versions_config();

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
        std::string url = "https://repo.amd.com/rocm/tarball-multi-arch/" + filename;

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

        {
            std::ofstream mf(fs::path(install_dir) / "method.txt");
            mf << "tarball";
        }

        // Write version.txt last for the same reason as the wheel path
        // (see install_therock_wheels()).
        std::ofstream vf(version_file);
        vf << version;
        vf.close();

        fs::remove(tarball_path);
        cleanup_old_therock_versions();

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

    std::vector<std::string> BackendUtils::get_therock_lib_paths(const std::string& rocm_arch) {
        const json& config = backend_versions_config();

        if (!config.contains("therock") || !config["therock"].contains("version")) {
            throw std::runtime_error("backend_versions.json is missing 'therock.version'");
        }

        std::string version = config["therock"]["version"].get<std::string>();

        std::string install_method = "auto";
        if (auto* cfg = RuntimeConfig::global()) {
            install_method = cfg->rocm_install_method();
        }

        // Prefer the lemonade-managed pip-wheel install when present (unless the
        // user pinned the tarball above). Its ROCm runtime is split across
        // multiple directories (_rocm_sdk_core/bin, _rocm_sdk_core/lib,
        // _rocm_sdk_libraries/bin), recorded in runtime_paths.txt at install
        // time. ALL of them must be on the loader path: amdhip64/amd_comgr/
        // rocm_kpack live in _rocm_sdk_core/bin while rocblas/hipblas/hipblaslt
        // live in _rocm_sdk_libraries/bin, and a consumer that loads only the
        // first directory fails to resolve the BLAS DLLs (STATUS_DLL_NOT_FOUND).
        if (install_method != "tarball") {
            fs::path paths_file =
                fs::path(get_therock_wheel_dir(rocm_arch, version)) / "runtime_paths.txt";
            std::error_code ec;
            if (fs::exists(paths_file, ec)) {
                std::ifstream pf(paths_file);
                std::string line;
                std::vector<std::string> lib_paths;
                auto add_dir = [&lib_paths](const std::string& dir) {
                    if (std::find(lib_paths.begin(), lib_paths.end(), dir) == lib_paths.end()) {
                        lib_paths.push_back(dir);
                    }
                };
                while (std::getline(pf, line)) {
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    if (line.empty()) {
                        continue;
                    }
                    std::error_code dir_ec;
                    // Decode UTF-8 so non-ASCII Windows profile paths aren't dropped.
                    const fs::path dir = utils::path_from_utf8(line);
                    if (fs::is_directory(dir, dir_ec)) {
                        add_dir(line);
                        // libomp lives in the LLVM runtime subdir, which older
                        // installs left out of runtime_paths.txt. Derive it from
                        // each recorded dir so a stale file still resolves OpenMP
                        // without a reinstall; the fixed probe records it too, so
                        // add_dir dedupes.
#ifdef _WIN32
                        const fs::path llvm = dir / "llvm" / "bin";
#else
                        const fs::path llvm = dir / "llvm" / "lib";
#endif
                        std::error_code llvm_ec;
                        if (fs::is_directory(llvm, llvm_ec) && !llvm_ec) {
                            add_dir(utils::path_to_utf8(llvm));
                        }
                    }
                }
                if (!lib_paths.empty()) {
                    LOG(DEBUG, "BackendUtils")
                        << "Returning " << lib_paths.size()
                        << " ROCm wheel runtime path(s); first: " << lib_paths.front() << std::endl;
                    return lib_paths;
                }
            }
        }

        // Tarball layout: a single runtime directory with a shared LLVM dir.
        std::string install_dir = get_therock_install_dir(rocm_arch, version);
        std::error_code ec;
        if (fs::exists(install_dir, ec)) {
            // On Windows, DLLs are in bin/ (lib/ contains only import .lib files)
            // On Linux, shared libraries are in lib/
            // Both are under the common tarball root which also keeps LLVM in lib/llvm/.
#ifdef _WIN32
            std::string lib_path = (fs::path(install_dir) / "bin").string();
#else
            std::string lib_path = (fs::path(install_dir) / "lib").string();
#endif
            // LLVM DLLs live in bin on Windows, .so files in lib on Linux.
#ifdef _WIN32
            fs::path llvm_path = fs::path(install_dir) / "lib" / "llvm" / "bin";
#else
            fs::path llvm_path = fs::path(install_dir) / "lib" / "llvm" / "lib";
#endif
            std::error_code llvm_ec;
            if (fs::is_directory(llvm_path, llvm_ec) && !llvm_ec) {
                LOG(DEBUG, "BackendUtils")
                    << "Returning TheRock runtime + LLVM paths: " << lib_path << ", "
                    << utils::path_to_utf8(llvm_path) << std::endl;
                return {lib_path, utils::path_to_utf8(fs::absolute(llvm_path, llvm_ec))};
            }
            LOG(DEBUG, "BackendUtils") << "Returning TheRock runtime path: " << lib_path << std::endl;
            return {lib_path};
        }

        return {};
    }

    std::string BackendUtils::get_therock_lib_path(const std::string& rocm_arch) {
        std::vector<std::string> paths = get_therock_lib_paths(rocm_arch);
        return paths.empty() ? std::string() : paths.front();
    }

    bool BackendUtils::therock_wheel_runtime_alive(const std::string& arch,
                                                   const std::string& version) {
        const fs::path paths_file =
            fs::path(get_therock_wheel_dir(arch, version)) / "runtime_paths.txt";
        std::error_code ec;
        if (!fs::exists(paths_file, ec)) {
            return false;
        }

        std::ifstream pf(paths_file);
        std::string line;
        while (std::getline(pf, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }
            std::error_code dir_ec;
            if (fs::is_directory(utils::path_from_utf8(line), dir_ec)) {
                return true;
            }
        }
        return false;
    }

    std::string BackendUtils::join_runtime_dirs(const std::vector<std::string>& dirs) {
#if !defined(__linux__) && !defined(_WIN32)
        (void)dirs;
        return "";
#else
#ifdef _WIN32
        const char sep = ';';
#else
        const char sep = ':';
#endif
        std::string out;
        auto add = [&](const fs::path& p) {
            if (p.empty()) {
                return;
            }
            std::error_code ec;
            std::string abs = utils::path_to_utf8(fs::absolute(p, ec));
            if (ec || abs.empty()) {
                return;
            }
            if (!out.empty()) {
                out += sep;
            }
            out += abs;
        };
        for (const auto& d : dirs) {
            if (d.empty()) {
                continue;
            }
            add(utils::path_from_utf8(d));
        }
        return out;
#endif
    }

#ifdef _WIN32
    // GetFileVersionInfoW can return stale cached data for system32 paths (it
    // reported 6.2 for a file that actually contains 10.0). Callers read
    // versions from plain paths (TheRock dir, staged copies) where it is
    // reliable.
    uint64_t BackendUtils::read_dll_version(const fs::path& dll) {
        const std::wstring wpath = dll.wstring();
        DWORD handle = 0;
        DWORD size = GetFileVersionInfoSizeW(wpath.c_str(), &handle);
        if (size == 0) {
            return 0;
        }
        std::vector<BYTE> data(size);
        if (!GetFileVersionInfoW(wpath.c_str(), handle, size, data.data())) {
            return 0;
        }
        VS_FIXEDFILEINFO* ffi = nullptr;
        UINT len = 0;
        if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&ffi), &len) || ffi == nullptr) {
            return 0;
        }
        return (static_cast<uint64_t>(ffi->dwFileVersionMS) << 32) | ffi->dwFileVersionLS;
    }
#else
    uint64_t BackendUtils::read_dll_version(const fs::path& dll) {
        (void)dll;
        return 0;
    }
#endif

    bool BackendUtils::stage_therock_hip_runtime(const std::string& rocm_arch,
                                                 const fs::path& target_dir) {
#ifndef _WIN32
        (void)rocm_arch;
        (void)target_dir;
        return false;
#else
        if (rocm_arch.empty()) {
            return false;
        }

        std::vector<std::string> therock_dirs = get_therock_lib_paths(rocm_arch);
        if (therock_dirs.empty()) {
            return false;
        }

        const fs::path therock_dll = utils::path_from_utf8(therock_dirs.front()) / "amdhip64_7.dll";
        if (!fs::exists(therock_dll)) {
            return false;
        }

        const fs::path target_dll = target_dir / "amdhip64_7.dll";

        wchar_t sysdir[MAX_PATH] = {};
        if (GetSystemDirectoryW(sysdir, MAX_PATH) == 0) {
            return false;
        }
        const fs::path system_dll = fs::path(sysdir) / "amdhip64_7.dll";

        const uint64_t therock_ver = read_dll_version(therock_dll);

        // A previously staged copy may be locked by a running backend process
        // (Windows blocks overwriting a loaded DLL), so leave it untouched when
        // it is already at least as new as TheRock's.
        const uint64_t staged_ver = read_dll_version(target_dll);
        if (staged_ver != 0 && staged_ver >= therock_ver) {
            LOG(INFO, "BackendUtils")
                << "Existing amdhip64_7.dll at " << utils::path_to_utf8(target_dll)
                << " is at least as new as TheRock's; leaving it in place" << std::endl;
            return false;
        }

        // Windows loads DLLs from the exe dir before System32, so the staged
        // copy wins over System32. Stage System32's runtime first (a plain
        // path, where GetFileVersionInfoW is reliable) and only overwrite it
        // with TheRock's when TheRock is newer.
        std::error_code ec;
        if (fs::exists(system_dll)) {
            fs::copy_file(system_dll, target_dll, fs::copy_options::overwrite_existing, ec);
            if (!ec) {
                const uint64_t system_ver = read_dll_version(target_dll);
                if (system_ver != 0 && system_ver >= therock_ver) {
                    LOG(INFO, "BackendUtils")
                        << "System32 amdhip64_7.dll is at least as new as TheRock's; staged it at "
                        << utils::path_to_utf8(target_dll) << std::endl;
                    return false;
                }
                fs::copy_file(therock_dll, target_dll, fs::copy_options::overwrite_existing, ec);
                if (!ec) {
                    LOG(INFO, "BackendUtils")
                        << "TheRock's amdhip64_7.dll is newer than System32's; staged it at "
                        << utils::path_to_utf8(target_dll) << std::endl;
                    return true;
                }
                LOG(ERROR, "BackendUtils")
                    << "Failed to copy amdhip64_7.dll from TheRock: " << ec.message() << std::endl;
                return false;
            }
            LOG(WARNING, "BackendUtils")
                << "Failed to stage System32 amdhip64_7.dll: " << ec.message() << std::endl;
        }

        fs::copy_file(therock_dll, target_dll, fs::copy_options::overwrite_existing, ec);
        if (!ec) {
            LOG(INFO, "BackendUtils")
                << "Copied amdhip64_7.dll from TheRock to " << utils::path_to_utf8(target_dll)
                << std::endl;
            return true;
        }
        LOG(ERROR, "BackendUtils") << "Failed to copy amdhip64_7.dll: " << ec.message() << std::endl;
        return false;
#endif
    }
    void BackendUtils::apply_cuda_env_vars(
            std::vector<std::pair<std::string, std::string>>& env_vars,
            const std::string& log_tag,
            bool skip_visible_devices) {
        if (!skip_visible_devices) {
            std::string existing_visible_devices = utils::get_environment_variable_utf8("CUDA_VISIBLE_DEVICES");
            const bool has_visible_override = !existing_visible_devices.empty();

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

    void BackendUtils::validate_device_backend_match(
            const std::string& backend,
            const std::string& target_device) {
        if (backend.empty() || backend == "auto" || backend == "system" || target_device.empty()) {
            return;
        }

        std::string lower_device = target_device;
        for (char& c : lower_device) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        std::string lower_backend = backend;
        for (char& c : lower_backend) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }

        const bool is_rocm_device = (lower_device.rfind("rocm", 0) == 0);
        const bool is_cuda_device = (lower_device.rfind("cuda", 0) == 0);
        const bool is_vulkan_device = (lower_device.rfind("vulkan", 0) == 0);

        const bool is_rocm_backend = (lower_backend.rfind("rocm", 0) == 0);
        const bool is_cuda_backend = (lower_backend.rfind("cuda", 0) == 0);
        const bool is_vulkan_backend = (lower_backend.rfind("vulkan", 0) == 0);

        if (is_rocm_device && !is_rocm_backend) {
            throw std::invalid_argument(
                "Device selection '" + target_device + "' contradicts backend choice '" + backend +
                "'. Expected a " + backend + " device identifier (e.g. " +
                (is_vulkan_backend ? "Vulkan0" : is_cuda_backend ? "CUDA0" : "a matching device") + ").");
        }
        if (is_cuda_device && !is_cuda_backend) {
            throw std::invalid_argument(
                "Device selection '" + target_device + "' contradicts backend choice '" + backend +
                "'. Expected a " + backend + " device identifier.");
        }
        if (is_vulkan_device && !is_vulkan_backend) {
            throw std::invalid_argument(
                "Device selection '" + target_device + "' contradicts backend choice '" + backend +
                "'. Expected a " + backend + " device identifier.");
        }
    }
} // namespace lemon::backends

#include "lemon/backends/backend_descriptor_registry.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/path_platform.h"
#include "lemon/utils/aixlog.hpp"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#else
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#endif

#include "backend_descriptors_generated.h"

namespace lemon {
namespace backends {

namespace fs = std::filesystem;

static std::shared_ptr<const BackendDescriptor> parse_descriptor_file(const fs::path& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) return nullptr;

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    nlohmann::json j = nlohmann::json::parse(content, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        LOG(WARNING, "BackendDescriptorRegistry") << "Skipping custom backend file " << filepath << ": invalid JSON content" << std::endl;
        return nullptr;
    }

    if (!j.contains("recipe") || !j["recipe"].is_string() ||
        !j.contains("display_name") || !j["display_name"].is_string() ||
        !j.contains("capabilities") || !j["capabilities"].is_array() ||
        !j.contains("platforms") || !j["platforms"].is_object()) {
        LOG(WARNING, "BackendDescriptorRegistry") << "Skipping custom backend file " << filepath << ": missing required fields (recipe, display_name, capabilities, platforms)" << std::endl;
        return nullptr;
    }

    auto desc = std::make_shared<BackendDescriptor>();
    desc->recipe = j["recipe"].get<std::string>();
    desc->display_name = j["display_name"].get<std::string>();
    desc->is_dynamic = true;

    if (j.contains("modality") && j["modality"].is_string()) {
        desc->modality = j["modality"].get<std::string>();
    }
    if (j.contains("slot_policy") && j["slot_policy"].is_string()) {
        std::string sp = j["slot_policy"].get<std::string>();
        if (sp == "exclusive_npu") desc->slot_policy = SlotPolicy::ExclusiveNpu;
        else if (sp == "coexist_by_type") desc->slot_policy = SlotPolicy::CoexistByType;
        else if (sp == "unmetered") desc->slot_policy = SlotPolicy::Unmetered;
        else desc->slot_policy = SlotPolicy::Standard;
    }

    for (const auto& cap : j["capabilities"]) {
        if (cap.is_string()) {
            desc->capabilities.push_back(cap.get<std::string>());
        }
    }

    if (j.contains("health_probe") && j["health_probe"].is_object()) {
        const auto& hp = j["health_probe"];
        if (hp.contains("type") && hp["type"].is_string()) desc->health_probe.type = hp["type"].get<std::string>();
        if (hp.contains("endpoint") && hp["endpoint"].is_string()) desc->health_probe.endpoint = hp["endpoint"].get<std::string>();
        if (hp.contains("expected_status") && hp["expected_status"].is_number_integer()) desc->health_probe.expected_status = hp["expected_status"].get<int>();
        if (hp.contains("timeout_seconds") && hp["timeout_seconds"].is_number_integer()) desc->health_probe.timeout_seconds = std::min(hp["timeout_seconds"].get<int>(), 300);
        if (hp.contains("poll_interval_ms") && hp["poll_interval_ms"].is_number_integer()) desc->health_probe.poll_interval_ms = hp["poll_interval_ms"].get<int>();
    }
    if (j.contains("health_endpoint") && j["health_endpoint"].is_string()) {
        desc->health_endpoint = j["health_endpoint"].get<std::string>();
        desc->health_probe.endpoint = desc->health_endpoint;
    }
    if (j.contains("health_timeout_seconds") && j["health_timeout_seconds"].is_number_integer()) {
        desc->health_timeout_seconds = std::min(j["health_timeout_seconds"].get<int>(), 300);
        desc->health_probe.timeout_seconds = desc->health_timeout_seconds;
    }
    if (j.contains("downsize_endpoint") && j["downsize_endpoint"].is_string()) {
        desc->downsize_endpoint = j["downsize_endpoint"].get<std::string>();
    }

    if (j.contains("endpoints") && j["endpoints"].is_object()) {
        for (auto ep_it = j["endpoints"].begin(); ep_it != j["endpoints"].end(); ++ep_it) {
            if (ep_it.value().is_string()) {
                desc->endpoints[ep_it.key()] = ep_it.value().get<std::string>();
            }
        }
    }

    if (j.contains("protected_flags") && j["protected_flags"].is_array()) {
        for (const auto& pf : j["protected_flags"]) {
            if (pf.is_string()) {
                desc->protected_flags.push_back(pf.get<std::string>());
            }
        }
    }

    if (j.contains("custom_options") && j["custom_options"].is_array()) {
        for (const auto& opt : j["custom_options"]) {
            if (!opt.is_object() || !opt.contains("name") || !opt.contains("cli_flag")) continue;
            BackendOption bo;
            bo.name = opt["name"].get<std::string>();
            bo.cli_flag = opt["cli_flag"].get<std::string>();
            if (opt.contains("default_value")) bo.default_value = opt["default_value"];
            if (opt.contains("type_name") && opt["type_name"].is_string()) bo.type_name = opt["type_name"].get<std::string>();
            if (opt.contains("help") && opt["help"].is_string()) bo.help = opt["help"].get<std::string>();
            if (opt.contains("group") && opt["group"].is_string()) bo.group = opt["group"].get<std::string>();
            desc->options.push_back(bo);
        }
    }

    for (auto it = j["platforms"].begin(); it != j["platforms"].end(); ++it) {
        std::string p_name = it.key();
        const auto& p_val = it.value();
        if (!p_val.is_object() || !p_val.contains("command") || !p_val.contains("args")) continue;

        BackendDescriptor::PlatformConfig pc;
        pc.command = p_val["command"].get<std::string>();

        if (p_val.contains("args") && p_val["args"].is_array()) {
            for (const auto& a : p_val["args"]) {
                if (a.is_string()) pc.args.push_back(a.get<std::string>());
            }
        }
        if (p_val.contains("stop_command") && p_val["stop_command"].is_string()) {
            pc.stop_command = p_val["stop_command"].get<std::string>();
        }
        if (p_val.contains("stop_command_args") && p_val["stop_command_args"].is_array()) {
            for (const auto& a : p_val["stop_command_args"]) {
                if (a.is_string()) pc.stop_command_args.push_back(a.get<std::string>());
            }
        }
        if (p_val.contains("env") && p_val["env"].is_object()) {
            for (auto env_it = p_val["env"].begin(); env_it != p_val["env"].end(); ++env_it) {
                if (env_it.value().is_string()) {
                    pc.env[env_it.key()] = env_it.value().get<std::string>();
                }
            }
        }

        desc->platforms[p_name] = pc;
    }

    if (desc->platforms.empty()) {
        return nullptr;
    }

    return desc;
}

static bool check_path_permissions(const fs::path& path, bool is_system_path) {
#ifndef _WIN32
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) {
        LOG(WARNING, "BackendDescriptorRegistry") << "Skipping descriptor '" << path.string() << "': failed to lstat file." << std::endl;
        return false;
    }
    if (S_ISLNK(st.st_mode)) {
        LOG(WARNING, "BackendDescriptorRegistry") << "Skipping descriptor '" << path.string() << "': symlinks are not permitted for descriptor files." << std::endl;
        return false;
    }
    if ((st.st_mode & 0022) != 0) {
        LOG(WARNING, "BackendDescriptorRegistry") << "Skipping descriptor '" << path.string() << "': file has group or world write permissions (mode mask 0022)." << std::endl;
        return false;
    }
    if (!is_system_path) {
        if (st.st_uid != geteuid()) {
            LOG(WARNING, "BackendDescriptorRegistry") << "Skipping descriptor '" << path.string() << "': owner UID (" << st.st_uid << ") does not match current user UID (" << geteuid() << ")." << std::endl;
            return false;
        }
    } else {
        if (st.st_uid != 0) {
            LOG(WARNING, "BackendDescriptorRegistry") << "Skipping system descriptor '" << path.string() << "': owner UID (" << st.st_uid << ") is not root (UID 0)." << std::endl;
            return false;
        }
    }

    std::error_code ec;
    fs::path target_path = fs::weakly_canonical(path, ec);
    if (ec) {
        target_path = path;
    }

    fs::path parent = target_path.parent_path();
    while (!parent.empty() && parent != parent.root_path()) {
        struct stat parent_st;
        if (::lstat(parent.c_str(), &parent_st) == 0) {
            if (S_ISLNK(parent_st.st_mode)) {
                LOG(WARNING, "BackendDescriptorRegistry") << "Skipping descriptor '" << path.string() << "': ancestor directory '" << parent.string() << "' is a symlink." << std::endl;
                return false;
            }
            if (parent_st.st_uid != geteuid() && (parent_st.st_mode & 0022) != 0 && (parent_st.st_mode & S_ISVTX) == 0) {
                LOG(WARNING, "BackendDescriptorRegistry") << "Skipping descriptor '" << path.string() << "': ancestor directory '" << parent.string() << "' owned by UID " << parent_st.st_uid << " is group/world writable without sticky bit set." << std::endl;
                return false;
            }
        }
        parent = parent.parent_path();
    }
#else
    PSECURITY_DESCRIPTOR pSD = nullptr;
    PACL pDacl = nullptr;
    PSID pOwnerSid = nullptr;
    BOOL ownerDefaulted = FALSE;

    DWORD res = GetNamedSecurityInfoW(
        path.wstring().c_str(),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | OWNER_SECURITY_INFORMATION,
        &pOwnerSid,
        nullptr,
        &pDacl,
        nullptr,
        &pSD
    );
    if (res != ERROR_SUCCESS || !pDacl || !pSD) {
        if (pSD) LocalFree(pSD);
        LOG(WARNING, "BackendDescriptorRegistry") << "Skipping descriptor '" << path.string() << "': failed to retrieve Windows security descriptor (error " << res << ")." << std::endl;
        return false;
    }

    if (pOwnerSid) {
        BYTE userSidBuffer[SECURITY_MAX_SID_SIZE];
        DWORD userSidSize = sizeof(userSidBuffer);
        HANDLE hToken = NULL;
        bool owner_ok = false;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
            TOKEN_USER* pTokenUser = reinterpret_cast<TOKEN_USER*>(userSidBuffer);
            if (GetTokenInformation(hToken, TokenUser, pTokenUser, userSidSize, &userSidSize)) {
                if (EqualSid(pOwnerSid, pTokenUser->User.Sid)) {
                    owner_ok = true;
                }
            }
            CloseHandle(hToken);
        }
        if (!owner_ok && IsWellKnownSid(pOwnerSid, WinBuiltinAdministratorsSid)) {
            owner_ok = true;
        }
        if (!owner_ok && is_system_path && IsWellKnownSid(pOwnerSid, WinLocalSystemSid)) {
            owner_ok = true;
        }
        if (!owner_ok) {
            LocalFree(pSD);
            LOG(WARNING, "BackendDescriptorRegistry") << "Skipping descriptor '" << path.string() << "': Windows security descriptor owner SID is not authorized for process." << std::endl;
            return false;
        }
    }

    ACL_SIZE_INFORMATION aclInfo;
    if (!GetAclInformation(pDacl, &aclInfo, sizeof(aclInfo), AclSizeInformation)) {
        LocalFree(pSD);
        LOG(WARNING, "BackendDescriptorRegistry") << "Skipping descriptor '" << path.string() << "': failed to retrieve GetAclInformation." << std::endl;
        return false;
    }

    for (DWORD i = 0; i < aclInfo.AceCount; ++i) {
        LPVOID pAce = nullptr;
        if (GetAce(pDacl, i, &pAce)) {
            ACE_HEADER* header = static_cast<ACE_HEADER*>(pAce);
            if (header->AceType == ACCESS_ALLOWED_ACE_TYPE || header->AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE) {
                ACCESS_MASK mask = 0;
                PSID sid = nullptr;

                if (header->AceType == ACCESS_ALLOWED_ACE_TYPE) {
                    ACCESS_ALLOWED_ACE* ace = static_cast<ACCESS_ALLOWED_ACE*>(pAce);
                    mask = ace->Mask;
                    sid = reinterpret_cast<PSID>(&ace->SidStart);
                } else {
                    ACCESS_ALLOWED_OBJECT_ACE* ace = static_cast<ACCESS_ALLOWED_OBJECT_ACE*>(pAce);
                    mask = ace->Mask;
                    BYTE* offset = reinterpret_cast<BYTE*>(&ace->ObjectType);
                    if (ace->Flags & ACE_OBJECT_TYPE_PRESENT) {
                        offset += sizeof(GUID);
                    }
                    if (ace->Flags & ACE_INHERITED_OBJECT_TYPE_PRESENT) {
                        offset += sizeof(GUID);
                    }
                    sid = reinterpret_cast<PSID>(offset);
                }

                if ((mask & (FILE_WRITE_DATA | FILE_APPEND_DATA | GENERIC_WRITE | WRITE_DAC | WRITE_OWNER)) != 0) {
                    if (sid != nullptr && IsValidSid(sid)) {
                        if (IsWellKnownSid(sid, WinWorldSid) || IsWellKnownSid(sid, WinBuiltinUsersSid)) {
                            LocalFree(pSD);
                            LOG(WARNING, "BackendDescriptorRegistry") << "Skipping descriptor '" << path.string() << "': Windows ACL grants write access to Everyone or Users group." << std::endl;
                            return false;
                        }
                    }
                }
            }
        }
    }
    LocalFree(pSD);
#endif
    return true;
}

static std::vector<fs::path> get_system_backend_search_paths() {
    std::vector<fs::path> paths;
#ifndef _WIN32
    paths.push_back("/usr/share/lemonade-server/backends");
    paths.push_back("/usr/local/share/lemonade-server/backends");
    paths.push_back("/Library/Application Support/Lemonade/backends");
    paths.push_back("/etc/lemonade/backends");
#else
    const char* prog_data = std::getenv("ProgramData");
    if (prog_data) {
        paths.push_back(fs::path(prog_data) / "Lemonade" / "backends");
    }
#endif
    return paths;
}

static std::vector<fs::path> get_backend_search_paths() {
    std::vector<fs::path> paths = get_system_backend_search_paths();

    paths.push_back(fs::path(utils::get_cache_dir()) / "backends");

#ifndef _WIN32
    const char* xdg_config = std::getenv("XDG_CONFIG_HOME");
    if (xdg_config && xdg_config[0] != '\0') {
        paths.push_back(fs::path(xdg_config) / "lemonade" / "backends");
    } else {
        const char* home = std::getenv("HOME");
        if (home) {
            paths.push_back(fs::path(home) / ".config" / "lemonade" / "backends");
        }
    }
#else
    const char* app_data = std::getenv("APPDATA");
    if (app_data) {
        paths.push_back(fs::path(app_data) / "Lemonade" / "backends");
    }
#endif

    return paths;
}

static void load_dynamic_descriptors(std::unordered_map<std::string, std::shared_ptr<const BackendDescriptor>>& dynamic_map) {
    std::vector<fs::path> search_paths = get_backend_search_paths();
    std::vector<fs::path> system_dirs = get_system_backend_search_paths();

    for (const auto& dir_path : search_paths) {
        std::error_code ec;
        if (!fs::exists(dir_path, ec) || !fs::is_directory(dir_path, ec)) {
            continue;
        }

        bool is_system = false;
        for (const auto& sys_dir : system_dirs) {
            if (fs::exists(sys_dir, ec) && fs::equivalent(dir_path, sys_dir, ec)) {
                is_system = true;
                break;
            }
        }

        for (const auto& entry : fs::directory_iterator(dir_path, ec)) {
            if (ec) break;
            if (entry.is_regular_file() && entry.path().extension() == ".json") {
                if (!check_path_permissions(entry.path(), is_system)) {
                    continue;
                }
                auto desc = parse_descriptor_file(entry.path());
                if (desc && !desc->recipe.empty()) {
                    dynamic_map[desc->recipe] = desc;
                }
            }
        }
    }
}

static std::mutex g_registry_mutex;
static bool g_initialized = false;
static std::vector<std::shared_ptr<const BackendDescriptor>> g_dynamic_storage;
static std::vector<const BackendDescriptor*> g_descriptors_cache;
static std::thread g_file_watcher_thread;
static std::atomic<bool> g_watcher_running{false};

void refresh_descriptors() {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    const std::vector<const BackendDescriptor*>& builtin = all_generated_descriptors();
    std::unordered_map<std::string, std::shared_ptr<const BackendDescriptor>> dynamic_map;

    load_dynamic_descriptors(dynamic_map);

    std::unordered_map<std::string, const BackendDescriptor*> merged_map;
    for (const auto* b : builtin) {
        merged_map[b->recipe] = b;
    }

    g_dynamic_storage.clear();
    for (const auto& [recipe, desc_ptr] : dynamic_map) {
        g_dynamic_storage.push_back(desc_ptr);
        merged_map[recipe] = desc_ptr.get();
    }

    g_descriptors_cache.clear();
    for (const auto& [recipe, ptr] : merged_map) {
        g_descriptors_cache.push_back(ptr);
    }
    g_initialized = true;
}

const std::vector<const BackendDescriptor*>& all_descriptors() {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    if (!g_initialized) {
        const std::vector<const BackendDescriptor*>& builtin = all_generated_descriptors();
        std::unordered_map<std::string, std::shared_ptr<const BackendDescriptor>> dynamic_map;

        load_dynamic_descriptors(dynamic_map);

        std::unordered_map<std::string, const BackendDescriptor*> merged_map;
        for (const auto* b : builtin) {
            merged_map[b->recipe] = b;
        }

        g_dynamic_storage.clear();
        for (const auto& [recipe, desc_ptr] : dynamic_map) {
            g_dynamic_storage.push_back(desc_ptr);
            merged_map[recipe] = desc_ptr.get();
        }

        g_descriptors_cache.clear();
        for (const auto& [recipe, ptr] : merged_map) {
            g_descriptors_cache.push_back(ptr);
        }
        g_initialized = true;
    }
    return g_descriptors_cache;
}

static uint64_t compute_search_paths_signature() {
    uint64_t sig = 0;
    std::vector<fs::path> search_paths = get_backend_search_paths();
    for (const auto& dir_path : search_paths) {
        std::error_code ec;
        if (fs::exists(dir_path, ec) && fs::is_directory(dir_path, ec)) {
            auto mtime = fs::last_write_time(dir_path, ec);
            if (!ec) {
                sig += std::chrono::duration_cast<std::chrono::milliseconds>(mtime.time_since_epoch()).count();
            }
            for (const auto& entry : fs::directory_iterator(dir_path, ec)) {
                if (ec) break;
                if (entry.is_regular_file() && entry.path().extension() == ".json") {
                    auto ftime = entry.last_write_time(ec);
                    if (!ec) {
                        sig += std::chrono::duration_cast<std::chrono::milliseconds>(ftime.time_since_epoch()).count();
                    }
                }
            }
        }
    }
    return sig;
}

void start_file_watcher() {
    if (g_watcher_running.exchange(true)) return;

    g_file_watcher_thread = std::thread([]() {
        uint64_t last_sig = compute_search_paths_signature();
        while (g_watcher_running) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (!g_watcher_running) break;
            uint64_t current_sig = compute_search_paths_signature();
            if (current_sig != last_sig) {
                last_sig = current_sig;
                LOG(INFO, "BackendDescriptorRegistry") << "Detected custom backend descriptor changes; reloading registry..." << std::endl;
                refresh_descriptors();
            }
        }
    });
}

void stop_file_watcher() {
    if (g_watcher_running.exchange(false)) {
        if (g_file_watcher_thread.joinable()) {
            g_file_watcher_thread.join();
        }
    }
}

const BackendDescriptor* descriptor_for(const std::string& recipe) {
    for (const BackendDescriptor* d : all_descriptors()) {
        if (d->recipe == recipe) {
            return d;
        }
    }
    return nullptr;
}

std::shared_ptr<const BackendDescriptor> descriptor_shared_for(const std::string& recipe) {
    all_descriptors();
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    for (const auto& ptr : g_dynamic_storage) {
        if (ptr && ptr->recipe == recipe) {
            return ptr;
        }
    }
    for (const BackendDescriptor* d : g_descriptors_cache) {
        if (d && d->recipe == recipe) {
            return std::shared_ptr<const BackendDescriptor>(std::shared_ptr<const BackendDescriptor>{}, d);
        }
    }
    return nullptr;
}

bool has_backend(const std::string& recipe) {
    return descriptor_for(recipe) != nullptr;
}

bool recipe_has_rocm_channels(const std::string& recipe) {
    const BackendDescriptor* d = descriptor_for(recipe);
    return d != nullptr && !d->rocm_channels.empty();
}

} // namespace backends
} // namespace lemon

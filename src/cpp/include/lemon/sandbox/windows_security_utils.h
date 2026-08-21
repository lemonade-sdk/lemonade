#pragma once

#ifdef _WIN32

#include "lemon/sandbox/sandbox_policy.h"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <userenv.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace lemon::sandbox {

class WindowsSecurityUtils {
public:
    static PSID derive_appcontainer_sid(const std::wstring& container_name);
    static void free_appcontainer_sid(PSID sid);
    static bool set_appcontainer_loopback_exemption(PSID sid, bool enable);
    static HANDLE create_sandboxed_job_object(bool active_process_limit_one = true);
    static DWORD64 get_standard_backend_mitigations();
};

class AclGrantGuard {
public:
    struct GrantEntry {
        std::wstring path;
        bool write_allowed;
    };

    AclGrantGuard(PSID app_container_sid, const std::vector<PathGrant>& grants);
    ~AclGrantGuard();

    AclGrantGuard(const AclGrantGuard&) = delete;
    AclGrantGuard& operator=(const AclGrantGuard&) = delete;
    AclGrantGuard(AclGrantGuard&&) = delete;
    AclGrantGuard& operator=(AclGrantGuard&&) = delete;

    const std::vector<GrantEntry>& applied_grants() const { return applied_grants_; }

private:
    PSID sid_{nullptr};
    std::vector<GrantEntry> applied_grants_;

    static bool grant_path_access(PSID sid, const std::wstring& path, bool write_allowed);
    static bool revoke_path_access(PSID sid, const std::wstring& path);
};

} // namespace lemon::sandbox

#endif // _WIN32

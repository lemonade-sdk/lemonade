#ifdef _WIN32

#include "lemon/sandbox/windows_security_utils.h"
#include "lemon/utils/aixlog.hpp"

#include <userenv.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "userenv.lib")

namespace lemon::sandbox {

PSID WindowsSecurityUtils::derive_appcontainer_sid(const std::wstring& container_name) {
    if (container_name.empty()) {
        return nullptr;
    }

    PSID sid = nullptr;
    HRESULT hr = DeriveAppContainerSidFromAppContainerName(container_name.c_str(), &sid);
    if (FAILED(hr) || sid == nullptr) {
        LOG(WARNING, "WindowsSecurityUtils")
            << "DeriveAppContainerSidFromAppContainerName failed with HRESULT: "
            << std::hex << hr << std::dec << std::endl;
        return nullptr;
    }
    return sid;
}

void WindowsSecurityUtils::free_appcontainer_sid(PSID sid) {
    if (sid != nullptr) {
        FreeSid(sid);
    }
}

typedef DWORD (WINAPI *pfnNetworkIsolationGetAppContainerConfig)(
    DWORD* pdwNumContainerSids,
    PSID_AND_ATTRIBUTES* appContainerSids
);

typedef DWORD (WINAPI *pfnNetworkIsolationSetAppContainerConfig)(
    DWORD dwNumContainerSids,
    PSID_AND_ATTRIBUTES appContainerSids
);

bool WindowsSecurityUtils::set_appcontainer_loopback_exemption(PSID sid, bool enable) {
    if (sid == nullptr) {
        return false;
    }

    HMODULE hFirewall = LoadLibraryW(L"FirewallAPI.dll");
    if (!hFirewall) {
        return false;
    }

    auto pGet = reinterpret_cast<pfnNetworkIsolationGetAppContainerConfig>(
        GetProcAddress(hFirewall, "NetworkIsolationGetAppContainerConfig"));
    auto pSet = reinterpret_cast<pfnNetworkIsolationSetAppContainerConfig>(
        GetProcAddress(hFirewall, "NetworkIsolationSetAppContainerConfig"));

    if (!pSet) {
        FreeLibrary(hFirewall);
        return false;
    }

    DWORD numExisting = 0;
    PSID_AND_ATTRIBUTES existingSids = nullptr;
    if (pGet) {
        pGet(&numExisting, &existingSids);
    }

    std::vector<SID_AND_ATTRIBUTES> updatedSids;
    bool alreadyExists = false;

    if (existingSids != nullptr && numExisting > 0) {
        for (DWORD i = 0; i < numExisting; ++i) {
            if (EqualSid(existingSids[i].Sid, sid)) {
                alreadyExists = true;
                if (enable) {
                    updatedSids.push_back(existingSids[i]);
                }
            } else {
                updatedSids.push_back(existingSids[i]);
            }
        }
    }

    if (enable && !alreadyExists) {
        SID_AND_ATTRIBUTES newAttr = {};
        newAttr.Sid = sid;
        newAttr.Attributes = 0;
        updatedSids.push_back(newAttr);
    }

    DWORD res = pSet(static_cast<DWORD>(updatedSids.size()), updatedSids.empty() ? nullptr : updatedSids.data());
    FreeLibrary(hFirewall);
    return (res == ERROR_SUCCESS);
}

HANDLE WindowsSecurityUtils::create_sandboxed_job_object(bool active_process_limit_one) {
    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    if (!hJob || hJob == INVALID_HANDLE_VALUE) {
        return nullptr;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limitInfo = {};
    limitInfo.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
        JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;

    if (active_process_limit_one) {
        limitInfo.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        limitInfo.BasicLimitInformation.ActiveProcessLimit = 1;
    }

    SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &limitInfo, sizeof(limitInfo));

    JOBOBJECT_BASIC_UI_RESTRICTIONS uiRestrictions = {};
    uiRestrictions.UIRestrictionsClass =
        JOB_OBJECT_UILIMIT_HANDLES |
        JOB_OBJECT_UILIMIT_GLOBALATOMS |
        JOB_OBJECT_UILIMIT_DESKTOP |
        JOB_OBJECT_UILIMIT_WRITECLIPBOARD;

    SetInformationJobObject(hJob, JobObjectBasicUIRestrictions, &uiRestrictions, sizeof(uiRestrictions));
    return hJob;
}

DWORD64 WindowsSecurityUtils::get_standard_backend_mitigations() {
    DWORD64 mitigations = 0;

#ifdef PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_REMOTE_ALWAYS_ON
    mitigations |= PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_REMOTE_ALWAYS_ON;
#endif
#ifdef PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_LOW_LABEL_ALWAYS_ON
    mitigations |= PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_LOW_LABEL_ALWAYS_ON;
#endif
#ifdef PROCESS_CREATION_MITIGATION_POLICY_FONT_DISABLE_ALWAYS_ON
    mitigations |= PROCESS_CREATION_MITIGATION_POLICY_FONT_DISABLE_ALWAYS_ON;
#endif
#ifdef PROCESS_CREATION_MITIGATION_POLICY_HIGH_ENTROPY_ASLR_ALWAYS_ON
    mitigations |= PROCESS_CREATION_MITIGATION_POLICY_HIGH_ENTROPY_ASLR_ALWAYS_ON;
#endif

    return mitigations;
}

AclGrantGuard::AclGrantGuard(PSID app_container_sid, const std::vector<PathGrant>& grants) {
    if (app_container_sid == nullptr) {
        return;
    }

    DWORD sid_len = GetLengthSid(app_container_sid);
    sid_ = LocalAlloc(LPTR, sid_len);
    if (!sid_ || !CopySid(sid_len, sid_, app_container_sid)) {
        if (sid_) {
            LocalFree(sid_);
            sid_ = nullptr;
        }
        return;
    }

    for (const auto& grant : grants) {
        if (grant.path.empty()) {
            continue;
        }

        std::filesystem::path p(grant.path);
        std::error_code ec;
        if (!std::filesystem::exists(p, ec)) {
            continue;
        }

        std::wstring wpath = p.wstring();
        if (grant_path_access(sid_, wpath, grant.write_allowed)) {
            applied_grants_.push_back({wpath, grant.write_allowed});
        }
    }
}

AclGrantGuard::~AclGrantGuard() {
    if (sid_ == nullptr) {
        return;
    }

    for (const auto& entry : applied_grants_) {
        revoke_path_access(sid_, entry.path);
    }
    LocalFree(sid_);
    sid_ = nullptr;
}

bool AclGrantGuard::grant_path_access(PSID sid, const std::wstring& path, bool write_allowed) {
    PACL pOldDACL = nullptr;
    PACL pNewDACL = nullptr;
    PSECURITY_DESCRIPTOR pSD = nullptr;

    DWORD get_res = GetNamedSecurityInfoW(
        path.c_str(),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        &pOldDACL,
        nullptr,
        &pSD
    );

    if (get_res != ERROR_SUCCESS) {
        return false;
    }

    EXPLICIT_ACCESS_W ea = {};
    ea.grfAccessPermissions = write_allowed
        ? (GENERIC_READ | GENERIC_WRITE | DELETE)
        : (GENERIC_READ | GENERIC_EXECUTE);
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);

    DWORD set_res = SetEntriesInAclW(1, &ea, pOldDACL, &pNewDACL);
    bool success = false;
    if (set_res == ERROR_SUCCESS && pNewDACL != nullptr) {
        DWORD apply_res = SetNamedSecurityInfoW(
            const_cast<LPWSTR>(path.c_str()),
            SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION,
            nullptr,
            nullptr,
            pNewDACL,
            nullptr
        );
        success = (apply_res == ERROR_SUCCESS);
        LocalFree(pNewDACL);
    }

    if (pSD != nullptr) {
        LocalFree(pSD);
    }

    return success;
}

bool AclGrantGuard::revoke_path_access(PSID sid, const std::wstring& path) {
    PACL pOldDACL = nullptr;
    PACL pNewDACL = nullptr;
    PSECURITY_DESCRIPTOR pSD = nullptr;

    DWORD get_res = GetNamedSecurityInfoW(
        path.c_str(),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        &pOldDACL,
        nullptr,
        &pSD
    );

    if (get_res != ERROR_SUCCESS) {
        return false;
    }

    EXPLICIT_ACCESS_W ea = {};
    ea.grfAccessPermissions = 0;
    ea.grfAccessMode = REVOKE_ACCESS;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);

    DWORD set_res = SetEntriesInAclW(1, &ea, pOldDACL, &pNewDACL);
    bool success = false;
    if (set_res == ERROR_SUCCESS && pNewDACL != nullptr) {
        DWORD apply_res = SetNamedSecurityInfoW(
            const_cast<LPWSTR>(path.c_str()),
            SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION,
            nullptr,
            nullptr,
            pNewDACL,
            nullptr
        );
        success = (apply_res == ERROR_SUCCESS);
        LocalFree(pNewDACL);
    }

    if (pSD != nullptr) {
        LocalFree(pSD);
    }

    return success;
}

} // namespace lemon::sandbox

#endif // _WIN32

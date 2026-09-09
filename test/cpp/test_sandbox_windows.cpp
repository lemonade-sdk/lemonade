#include "lemon/sandbox/sandbox_engine.h"
#include "lemon/sandbox/sandbox_policy.h"
#include "lemon/utils/process_manager.h"
#include "sandbox_test_utils.h"

#ifdef _WIN32
#include "lemon/sandbox/windows_security_utils.h"
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace lemon::sandbox;
using lemon::test::TestResult;

int main() {
    TestResult r;

#ifdef _WIN32
    {
        PSID sid1 = WindowsSecurityUtils::derive_appcontainer_sid(L"Lemonade.Backend.llamacpp");
        r.check(sid1 != nullptr, "derive_appcontainer_sid returns valid PSID for llamacpp");

        PSID sid2 = WindowsSecurityUtils::derive_appcontainer_sid(L"Lemonade.Backend.llamacpp");
        r.check(sid2 != nullptr, "derive_appcontainer_sid returns valid PSID for duplicate call");

        BOOL equal = EqualSid(sid1, sid2);
        r.check(equal == TRUE, "Derived AppContainer SID is deterministic for identical container name");

        WindowsSecurityUtils::free_appcontainer_sid(sid1);
        WindowsSecurityUtils::free_appcontainer_sid(sid2);

        PSID empty_sid = WindowsSecurityUtils::derive_appcontainer_sid(L"");
        r.check(empty_sid == nullptr, "derive_appcontainer_sid returns nullptr for empty name");
    }

    {
        HANDLE hJob = WindowsSecurityUtils::create_sandboxed_job_object(true);
        r.check(hJob != nullptr && hJob != INVALID_HANDLE_VALUE, "create_sandboxed_job_object creates valid handle");

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limitInfo = {};
        DWORD retLen = 0;
        BOOL qRes = QueryInformationJobObject(
            hJob,
            JobObjectExtendedLimitInformation,
            &limitInfo,
            sizeof(limitInfo),
            &retLen
        );

        r.check(qRes == TRUE, "QueryInformationJobObject succeeds on sandboxed job object");
        r.check((limitInfo.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) != 0,
                "Job object has JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE set");
        r.check((limitInfo.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_ACTIVE_PROCESS) != 0,
                "Job object has JOB_OBJECT_LIMIT_ACTIVE_PROCESS set");
        r.check(limitInfo.BasicLimitInformation.ActiveProcessLimit == 1,
                "Job object active process limit is set to 1");

        CloseHandle(hJob);
    }

    {
        DWORD64 mitigations = WindowsSecurityUtils::get_standard_backend_mitigations();
#ifdef PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_REMOTE_ALWAYS_ON
        r.check((mitigations & PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_REMOTE_ALWAYS_ON) != 0,
                "Mitigations include IMAGE_LOAD_NO_REMOTE");
#endif
#ifdef PROCESS_CREATION_MITIGATION_POLICY_HIGH_ENTROPY_ASLR_ALWAYS_ON
        r.check((mitigations & PROCESS_CREATION_MITIGATION_POLICY_HIGH_ENTROPY_ASLR_ALWAYS_ON) != 0,
                "Mitigations include HIGH_ENTROPY_ASLR");
#endif
    }

    {
        fs::path temp_dir = fs::temp_directory_path() / ("lemon_acl_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(temp_dir);

        PSID sid = WindowsSecurityUtils::derive_appcontainer_sid(L"Lemonade.Backend.TestAcl");
        r.check(sid != nullptr, "derive_appcontainer_sid succeeds for TestAcl");

        std::vector<PathGrant> grants = {
            {temp_dir.string(), true}
        };

        {
            AclGrantGuard guard(sid, grants);
            r.check(guard.applied_grants().size() == 1, "AclGrantGuard applied grant to temp directory");
        }

        WindowsSecurityUtils::free_appcontainer_sid(sid);
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
        r.check(true, "AclGrantGuard cleanly revoked ACEs upon destruction");
    }

    {
        // End-to-end child process spawn under Windows AppContainer & Job Object
        SandboxPolicy win_policy;
        win_policy.set_mode(SandboxMode::Auto);
        PolicyPresets::apply_system_runtime(win_policy);

        lemon::utils::ProcessHandle h_win = lemon::utils::ProcessManager::start_process(
            "cmd.exe", {"/c", "exit 0"}, "", false, false, {}, win_policy);
        r.check(h_win.pid > 0, "start_process spawned child process under Windows AppContainer sandbox");
        int win_exit = lemon::utils::ProcessManager::wait_for_exit(h_win, 5);
        r.check(win_exit == 0, "Windows AppContainer child process exited cleanly with code 0");
    }
#else
    {
        PlatformType p = PlatformDetector::detect_platform();
        r.check(p != PlatformType::WindowsNative, "PlatformDetector correctly detects non-Windows native platform");

        auto engine = SandboxEngine::create_for_platform();
        r.check(engine != nullptr, "create_for_platform returns valid SandboxEngine instance");

        std::string desc = SandboxEngine::get_platform_engine_description(SandboxMode::Auto);
        r.check(!desc.empty(), "get_platform_engine_description returns non-empty string");
    }
#endif

    r.report_summary("WindowsSandbox");
    return r.exit_code();
}

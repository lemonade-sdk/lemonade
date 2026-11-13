#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <lemon/utils/process_platform.h>

#include "windows_argv_cases.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using nlohmann::json;

int passed = 0;
int failed = 0;

void check(const std::string& name, bool condition) {
    std::cout << (condition ? "PASS " : "FAIL ") << name << '\n';
    condition ? ++passed : ++failed;
}

class EnvironmentGuard {
public:
    EnvironmentGuard(const wchar_t* name, const wchar_t* value) : name_(name) {
        SetLastError(ERROR_SUCCESS);
        const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
        present_ = size != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
        if (size != 0) {
            old_.resize(size);
            old_.resize(GetEnvironmentVariableW(name, old_.data(), size));
        }
        if (!SetEnvironmentVariableW(name, value)) {
            throw std::runtime_error("Cannot set test environment");
        }
    }

    ~EnvironmentGuard() {
        SetEnvironmentVariableW(name_.c_str(), present_ ? old_.c_str() : nullptr);
    }

private:
    std::wstring name_;
    std::wstring old_;
    bool present_ = false;
};

struct TemporaryDirectory {
    fs::path path = fs::temp_directory_path() /
        (L"lemonade argv \u6a21\u578b \U0001f600 " + std::to_wstring(GetCurrentProcessId()) +
         L"-" + std::to_wstring(GetTickCount64()));

    TemporaryDirectory() {
        fs::create_directory(path);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

void verify_report(const std::string& mode, const json& result, const fs::path& executable, const fs::path& cwd,
                   const std::string& mixed, const std::string& unicode) {
    check(mode + " Unicode argv[0]", result.at("argv0") == executable.u8string());
    check(mode + " argument count", result.at("argv").size() == std::size(windows_arg_cases));
    size_t index = 0;
    for (const auto& test : windows_arg_cases) {
        check(mode + " " + test.name, result.at("argv").at(index++) == test.input);
    }
    check(mode + " Unicode cwd", result.at("cwd") == cwd.u8string());
    check(mode + " inherited Unicode environment",
          result.at("inherited") == u8"inherited \u017c\u6a21\u578b\U0001f600");
    check(mode + " case-insensitive environment override", result.at("mixed") == mixed);
    check(mode + " Unicode environment name/value", result.at("unicode") == unicode);
}

}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        return 2;
    }
    try {
        EnvironmentGuard inherited(L"LEMONADE_ARGV_INHERITED", L"inherited \u017c\u6a21\u578b\U0001f600");
        EnvironmentGuard mixed(L"Lemonade_Argv_Mixed", L"parent");
        EnvironmentGuard unicode(L"LEMONADE_ARGV_\u6a21\u578b", L"parent \u6a21\u578b");
        TemporaryDirectory directory;
        const fs::path executable = directory.path / L"echo child \u017c\u6a21\u578b\U0001f600.exe";
        fs::copy_file(fs::path(argv[1]), executable);
        const fs::path cwd = directory.path / L"cwd \u6a21\u578b\U0001f600";
        fs::create_directory(cwd);
        auto platform = lemon::utils::create_process_platform();
        const std::vector<std::pair<std::string, std::string>> overrides = {
            {"lemonade_argv_mixed", u8"override \u017c\u6a21\u578b\U0001f600"},
            {u8"LEMONADE_ARGV_\u6a21\u578b", u8"value \u6a21\u578b\U0001f600"},
        };
        const std::vector<std::pair<std::string, std::string>> no_overrides;
        for (int mode = 0; mode < 4; ++mode) {
            const std::string label = "spawn " + std::to_string(mode);
            const fs::path report = directory.path / (L"report " + std::to_wstring(mode) + L".json");
            std::vector<std::string> args = {"--report-file", report.u8string()};
            for (const auto& test : windows_arg_cases) {
                args.emplace_back(test.input);
            }
            const auto handle = platform->spawn(executable.u8string(), args, cwd.u8string(),
                                                 mode >= 2, mode == 2,
                                                 mode == 0 ? no_overrides : overrides);
            const int exit_code = platform->wait_for_exit(handle, 10);
            check(label + " exit code", exit_code == 0);
            if (exit_code == -1) {
                platform->terminate_without_cleanup(handle);
                platform->wait_for_exit(handle, 5);
            }
            check(label + " stopped", !platform->is_running(handle));
            check(label + " reap", platform->reap(handle) == exit_code);
            std::ifstream input(report, std::ios::binary);
            const auto result = json::parse(input);
            verify_report(label, result, executable, cwd,
                          mode == 0 ? "parent" : overrides[0].second,
                          mode == 0 ? u8"parent \u6a21\u578b" : overrides[1].second);
        }
        for (bool capture_stderr : {false, true}) {
            const std::string label = capture_stderr ? "capture stderr" : "capture stdout";
            const fs::path report = directory.path / (capture_stderr ? L"capture1.json" : L"capture0.json");
            std::vector<std::string> args = {"--report-file", report.u8string()};
            for (const auto& test : windows_arg_cases) {
                args.emplace_back(test.input);
            }
            std::string output;
            const int exit_code = platform->run_with_output(executable.u8string(), args,
                [&output](const std::string& line) {
                    output += line;
                    return true;
                }, cwd.u8string(), 10, capture_stderr);
            check(label + " exit code", exit_code == 0);
            std::ifstream input(report, std::ios::binary);
            const auto result = json::parse(input);
            check(label + " stdout equals file", json::parse(output) == result);
            verify_report(label, result, executable, cwd, "parent", u8"parent \u6a21\u578b");
        }
        const fs::path shell_report = directory.path / L"shell \u6a21\u578b.txt";
        std::string shell_output;
        check("shell capture Unicode path", platform->run_command(
            "echo shell-ok > \"" + shell_report.u8string() + "\"", shell_output, 10) == 0);
        std::ifstream shell_input(shell_report);
        std::string shell_line;
        std::getline(shell_input, shell_line);
        check("shell capture redirected output", shell_line.find("shell-ok") == 0);
        const auto handle = platform->spawn(executable.u8string(), {"--wait"}, cwd.u8string(),
                                             false, false, {});
        check("lifecycle running", platform->is_running(handle));
        check("lifecycle nonblocking reap", platform->reap(handle) == -1);
        platform->terminate_without_cleanup(handle);
        check("lifecycle terminated exit", platform->wait_for_exit(handle, 5) == 1);
        check("lifecycle stopped", !platform->is_running(handle));
        check("lifecycle reap exit", platform->reap(handle) == 1);
    } catch (const std::exception& error) {
        check(std::string("exception: ") + error.what(), false);
    }
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
#endif

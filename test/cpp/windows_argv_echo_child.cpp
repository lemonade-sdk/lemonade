#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

std::string to_utf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int length = static_cast<int>(value.size());
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                        value.data(), length, nullptr, 0, nullptr, nullptr);
    if (size == 0) {
        throw std::runtime_error("Invalid UTF-16 child input");
    }
    std::string result(size, '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length,
                            result.data(), size, nullptr, nullptr) != size) {
        throw std::runtime_error("Child UTF-8 conversion failed");
    }
    return result;
}

std::string read_environment(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) {
        return {};
    }
    std::wstring value(size, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(), size);
    if (length >= size) {
        throw std::runtime_error("Child environment changed while reading");
    }
    value.resize(length);
    return to_utf8(value);
}

}

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--wait") {
        Sleep(30000);
        return 0;
    }
    if (argc < 3 || std::wstring(argv[1]) != L"--report-file") {
        return 2;
    }
    try {
        nlohmann::json result;
        result["argv0"] = to_utf8(argv[0]);
        result["argv"] = nlohmann::json::array();
        for (int i = 3; i < argc; ++i) {
            result["argv"].push_back(to_utf8(argv[i]));
        }
        result["cwd"] = std::filesystem::current_path().u8string();
        result["inherited"] = read_environment(L"LEMONADE_ARGV_INHERITED");
        result["mixed"] = read_environment(L"Lemonade_Argv_Mixed");
        result["unicode"] = read_environment(L"LEMONADE_ARGV_\u6a21\u578b");
        const auto serialized = result.dump(-1, ' ', true);
        std::ofstream report(std::filesystem::path(argv[2]), std::ios::binary);
        report << serialized;
        report.close();
        if (!report) {
            return 3;
        }
        std::cout << serialized << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 4;
    }
}
#endif

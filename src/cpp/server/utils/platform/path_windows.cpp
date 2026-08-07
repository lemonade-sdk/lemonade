#include <lemon/utils/path_platform.h>
#include <windows.h>
#include <stdexcept>

namespace lemon::utils {

namespace fs = std::filesystem;

class WindowsPathPlatform : public PathPlatform {
private:
    static std::wstring utf8_to_wstring(const std::string& str) {
        if (str.empty()) return std::wstring();

        int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
        if (size_needed <= 0) {
            return std::wstring();
        }

        std::wstring result(size_needed, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &result[0], size_needed);
        result.resize(size_needed - 1);
        return result;
    }

    static std::string wstring_to_utf8(const std::wstring& wstr) {
        if (wstr.empty()) return std::string();

        int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (size_needed <= 0) {
            return std::string();
        }

        std::string result(size_needed, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &result[0], size_needed, nullptr, nullptr);
        result.resize(size_needed - 1);
        return result;
    }

public:
    std::string get_environment_variable_utf8(const std::string& name) override {
        std::wstring wide_name = utf8_to_wstring(name);
        DWORD size_needed = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
        if (size_needed == 0) {
            return "";
        }

        std::wstring value(size_needed, L'\0');
        GetEnvironmentVariableW(wide_name.c_str(), &value[0], size_needed);
        value.resize(size_needed - 1);
        return wstring_to_utf8(value);
    }

    fs::path path_from_utf8(const std::string& path) override {
        return fs::u8path(path);
    }

    std::string path_to_utf8(const fs::path& path) override {
        return wstring_to_utf8(path.wstring());
    }

    std::string get_executable_dir() override {
        char buffer[MAX_PATH];
        GetModuleFileNameA(NULL, buffer, MAX_PATH);
        fs::path exe_path(buffer);
        return exe_path.parent_path().string();
    }

    std::string get_cache_dir(const std::string& g_cache_dir) override {
        if (!g_cache_dir.empty()) {
            return g_cache_dir;
        }

        std::string userprofile = get_environment_variable_utf8("USERPROFILE");
        if (!userprofile.empty()) {
            return userprofile + "\\.cache\\lemonade";
        }
        throw std::runtime_error("USERPROFILE is not set; cannot resolve Lemonade cache directory");
    }

    std::string get_runtime_dir() override {
        char temp_path[MAX_PATH];
        GetTempPathA(MAX_PATH, temp_path);
        return std::string(temp_path);
    }

    std::vector<std::string> get_install_prefixes() override {
        // Windows doesn't use FHS-style install prefixes
        return {};
    }

    std::string default_hf_cache_dir() override {
        std::string userprofile = get_environment_variable_utf8("USERPROFILE");
        if (!userprofile.empty()) {
            return userprofile + "\\.cache\\huggingface\\hub";
        }
        throw std::runtime_error("USERPROFILE is not set; cannot resolve HuggingFace cache directory");
    }
};

std::unique_ptr<PathPlatform> create_path_platform() {
    return std::make_unique<WindowsPathPlatform>();
}

} // namespace lemon::utils

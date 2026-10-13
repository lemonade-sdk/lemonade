#include <lemon/utils/json_file.h>
#include <lemon/utils/json_utils.h>
#include <lemon/utils/path_utils.h>
#include <lemon/utils/safe_fs.h>
#include <lemon/utils/aixlog.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;

namespace lemon {
namespace utils {

json load_optional_json(const std::string& path) {
    if (!fs::exists(path)) {
        return json::object();
    }

    try {
        LOG(INFO, "ModelManager") << "Loading " << fs::path(path).filename() << std::endl;
        return JsonUtils::load_from_file(path);
    } catch (const std::exception& e) {
        LOG(WARNING, "ModelManager") << "Could not load " << fs::path(path).filename() << ": " << e.what() << std::endl;
        return json::object();
    }
}

void save_json_atomic(const std::string& path, const json& value) {
    fs::path target = path_from_utf8(path);
    ensure_create_directories(target.parent_path());

    LOG(INFO, "ModelManager") << "Saving " << target.filename() << std::endl;

    std::ostringstream tmp_suffix;
    tmp_suffix << ".tmp."
               << std::this_thread::get_id() << "."
               << std::chrono::steady_clock::now().time_since_epoch().count() << "."
               << reinterpret_cast<std::uintptr_t>(&value);
    fs::path tmp = target;
    tmp += tmp_suffix.str();
    {
        std::ofstream file(tmp, std::ios::trunc);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open file for writing: " + path_to_utf8(tmp));
        }
        try {
            file << value.dump(2);
            file.flush();
        } catch (const json::exception& e) {
            throw std::runtime_error("Failed to write JSON to file " + path_to_utf8(tmp) + ": " + e.what());
        }
        if (!file) {
            throw std::runtime_error("Failed to flush JSON file: " + path_to_utf8(tmp));
        }
    }

    std::error_code ec;
    fs::rename(tmp, target, ec);
#ifdef _WIN32
    if (ec) {
        ec.clear();
        fs::remove(target, ec);
        ec.clear();
        fs::rename(tmp, target, ec);
    }
#endif
    if (ec) {
        std::error_code cleanup_ec;
        fs::remove(tmp, cleanup_ec);
        throw std::runtime_error("Failed to replace JSON file " + path + ": " + ec.message());
    }
}

}  // namespace utils
}  // namespace lemon

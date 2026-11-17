#include "lemon/server/video_store.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <fstream>
#include <iterator>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include <lemon/utils/aixlog.hpp>

#include "lemon/utils/path_utils.h"

namespace lemon {
namespace video_store {

namespace fs = std::filesystem;

namespace {
const std::string kPrefix = "video-";
constexpr size_t kHexLength = 32;

std::mutex& reservation_mutex() {
    static std::mutex m;
    return m;
}

std::set<std::string>& reserved() {
    static std::set<std::string> names;
    return names;
}
} // namespace

fs::path dir() {
    return utils::path_from_utf8(utils::get_cache_dir()) / "videos";
}

std::string new_file_name() {
    std::random_device device;
    std::seed_seq seed{device(), device(), device(), device()};
    std::mt19937_64 engine(seed);
    std::ostringstream name;
    name << kPrefix << std::hex;
    for (int i = 0; i < 2; ++i) {
        name.width(16);
        name.fill('0');
        name << engine();
    }
    return name.str();
}

bool is_valid_file_name(const std::string& name) {
    if (name.size() != kPrefix.size() + kHexLength || name.compare(0, kPrefix.size(), kPrefix) != 0)
        return false;
    return std::all_of(name.begin() + kPrefix.size(), name.end(),
                       [](unsigned char c) { return std::isxdigit(c) && !std::isupper(c); });
}

void write(const std::string& name, const std::string& bytes) {
    if (!is_valid_file_name(name)) throw std::runtime_error("invalid video file name");
    std::error_code ec;
    fs::create_directories(dir(), ec);
    std::ofstream out(dir() / name, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) throw std::runtime_error("could not write the video to " + utils::path_to_utf8(dir()));
}

bool read(const std::string& name, std::string& bytes) {
    if (!is_valid_file_name(name)) return false;
    std::ifstream in(dir() / name, std::ios::binary);
    if (!in) return false;
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool exists(const std::string& name) {
    std::error_code ec;
    return is_valid_file_name(name) && fs::exists(dir() / name, ec);
}

std::string reserve_file_name(const std::string& preferred) {
    std::lock_guard<std::mutex> lock(reservation_mutex());
    std::string name = preferred;
    while (!is_valid_file_name(name) || reserved().count(name) || exists(name)) name = new_file_name();
    reserved().insert(name);
    return name;
}

void release(const std::string& name) {
    std::lock_guard<std::mutex> lock(reservation_mutex());
    reserved().erase(name);
}

void remove_all_except(const std::set<std::string>& keep) {
    std::lock_guard<std::mutex> lock(reservation_mutex());
    for (const auto& name : keep) reserved().erase(name);
    std::error_code ec;
    for (fs::directory_iterator it(dir(), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = utils::path_to_utf8(it->path().filename());
        if (!is_valid_file_name(name) || keep.count(name) || reserved().count(name)) continue;
        std::error_code rm;
        fs::remove(it->path(), rm);
        if (rm) LOG(WARNING, "Videos") << "Could not remove " << name << ": " << rm.message() << std::endl;
    }
}

std::string normalize_request(nlohmann::json& request) {
    struct IntegerField {
        const char* name;
        double min;
        double max;
    };
    const IntegerField integers[] = {
        {"video_frames", 1, kMaxVideoFrames},
        {"fps", 1, kMaxFps},
        {"width", 1, kMaxDimension},
        {"height", 1, kMaxDimension},
        {"steps", 1, kMaxSteps},
        {"seed", static_cast<double>(std::numeric_limits<int64_t>::min()),
         static_cast<double>(std::numeric_limits<int64_t>::max())},
    };
    for (const auto& field : integers) {
        if (!request.contains(field.name) || request[field.name].is_null()) continue;
        auto& value = request[field.name];
        if (!value.is_number()) return std::string("'") + field.name + "' must be an integer";
        if (value.is_number_integer()) {
            const double number = value.get<double>();
            if (number < field.min || number > field.max)
                return std::string("'") + field.name + "' is out of range";
            continue;
        }
        const double number = value.get<double>();
        if (!std::isfinite(number) || std::floor(number) != number)
            return std::string("'") + field.name + "' must be an integer";
        if (number < field.min || number > field.max)
            return std::string("'") + field.name + "' is out of range";
        value = static_cast<int64_t>(number);
    }
    struct NumberField {
        const char* name;
        double min;
        double max;
    };
    const NumberField numbers[] = {
        {"duration", 0, kMaxDuration},
        {"cfg_scale", -std::numeric_limits<double>::max(), std::numeric_limits<double>::max()},
        {"flow_shift", -std::numeric_limits<double>::max(), std::numeric_limits<double>::max()},
    };
    for (const auto& field : numbers) {
        if (!request.contains(field.name) || request[field.name].is_null()) continue;
        const auto& value = request[field.name];
        if (!value.is_number() || !std::isfinite(value.get<double>()))
            return std::string("'") + field.name + "' must be a number";
        if (value.get<double>() < field.min || value.get<double>() > field.max)
            return std::string("'") + field.name + "' is out of range";
    }
    return "";
}

bool is_video_job(const nlohmann::json& job) {
    if (!job.is_object() || !job.contains("steps") || !job["steps"].is_array() ||
        job["steps"].size() != 1) {
        return false;
    }
    const auto& step = job["steps"][0];
    return step.is_object() && step.value("op", std::string()) == "generate_video";
}

std::string video_status(const nlohmann::json& job) {
    const std::string status = job.value("status", std::string());
    if (status == "paused" && is_video_job(job) &&
        job["steps"][0].value("status", std::string()) == "completed") {
        return "completed";
    }
    if (status != "interrupted") return status;
    return job.value("error", std::string()).empty() ? "canceled" : "failed";
}

} // namespace video_store
} // namespace lemon

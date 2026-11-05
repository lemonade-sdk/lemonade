#include "lemon/server/video_store.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
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

void remove_all_except(const std::set<std::string>& keep) {
    std::error_code ec;
    for (fs::directory_iterator it(dir(), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = utils::path_to_utf8(it->path().filename());
        if (!is_valid_file_name(name) || keep.count(name)) continue;
        std::error_code rm;
        fs::remove(it->path(), rm);
        if (rm) LOG(WARNING, "Videos") << "Could not remove " << name << ": " << rm.message() << std::endl;
    }
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

#include "lemon/server/video_store.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <fstream>
#include <iterator>
#include <map>
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

// File name -> the job it was reserved for. An empty id means the input is
// being staged before a job id exists; its staging guard owns the reservation.
std::map<std::string, std::string>& reserved() {
    static std::map<std::string, std::string> names;
    return names;
}
const std::string kImageSuffix = ".image.b64";

bool is_image_file_name(const std::string& name) {
    return name.size() == kPrefix.size() + kHexLength + kImageSuffix.size() &&
           name.compare(name.size() - kImageSuffix.size(), kImageSuffix.size(), kImageSuffix) == 0 &&
           is_valid_file_name(name.substr(0, name.size() - kImageSuffix.size()));
}

std::string validate_image(const std::string& image) {
    if (image.compare(0, 5, "data:") == 0)
        return "'image' must be raw base64; data URLs are not accepted";
    if (image.size() > kMaxImageBase64Bytes)
        return "'image' exceeds the 8 MiB encoded limit";
    if (image.empty() || image.size() % 4 != 0)
        return "'image' must be non-empty raw base64 (not a URL or path)";
    const auto pad = image.find('=');
    const auto data_size = pad == std::string::npos ? image.size() : pad;
    if (data_size < 2 || image.size() - data_size > 2 ||
        !std::all_of(image.begin(), image.begin() + data_size, [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') || c == '+' || c == '/';
        }) ||
        !std::all_of(image.begin() + data_size, image.end(), [](char c) { return c == '='; }))
        return "'image' must be raw base64 (not a URL or path)";
    return {};
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

namespace {
void write_bytes(const std::string& name, const std::string& bytes) {
    std::error_code ec;
    fs::create_directories(dir(), ec);
    std::ofstream out(dir() / name, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) throw std::runtime_error("could not write video file to " + utils::path_to_utf8(dir()));
}
} // namespace

void write(const std::string& name, const std::string& bytes) {
    if (!is_valid_file_name(name)) throw std::runtime_error("invalid video file name");
    write_bytes(name, bytes);
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

std::string reserve_file_name(const std::string& preferred, const std::string& job_id) {
    std::lock_guard<std::mutex> lock(reservation_mutex());
    std::string name = preferred;
    while (!is_valid_file_name(name) || reserved().count(name) || exists(name)) name = new_file_name();
    reserved()[name] = job_id;
    return name;
}

void release(const std::string& name) {
    std::lock_guard<std::mutex> lock(reservation_mutex());
    reserved().erase(name);
}

StagedImages::~StagedImages() {
    for (const auto& name : names_) release(name);
}

std::string StagedImages::stage(const std::string& base64) {
    if (const auto error = validate_image(base64); !error.empty())
        throw std::invalid_argument(error);
    std::string name;
    {
        std::lock_guard<std::mutex> lock(reservation_mutex());
        do {
            name = new_file_name() + kImageSuffix;
        } while (reserved().count(name) || fs::exists(dir() / name));
        names_.push_back(name);
        reserved()[name] = "";
    }
    // The backend needs base64, so keep the encoded bytes on disk and do not
    // decode/re-encode them. The job record contains only the short file name.
    write_bytes(name, base64);
    return name;
}

std::string restore_image(nlohmann::json& request) {
    const auto ref = request.find("image_file");
    if (ref == request.end()) return {};
    if (!ref->is_string() || !is_image_file_name(ref->get_ref<const std::string&>()))
        return "invalid stored video image reference";
    if (request.contains("image") && !request["image"].is_null())
        return "conflicting inline and stored video image";

    std::ifstream input(dir() / ref->get<std::string>(), std::ios::binary | std::ios::ate);
    if (!input) return "stored video image is missing";
    const auto size = input.tellg();
    if (size <= 0 || size > static_cast<std::streamoff>(kMaxImageBase64Bytes))
        return "stored video image is empty or exceeds the 8 MiB encoded limit";
    std::string image(static_cast<std::size_t>(size), '\0');
    input.seekg(0);
    if (!input.read(image.data(), static_cast<std::streamsize>(image.size())))
        return "could not read stored video image";
    request.erase("image_file");
    request["image"] = std::move(image);
    return {};
}

void clean_up(const std::function<CleanupSnapshot()>& snapshot) {
    std::lock_guard<std::mutex> lock(reservation_mutex());
    const CleanupSnapshot now = snapshot();
    for (auto it = reserved().begin(); it != reserved().end();) {
        const bool published = !it->second.empty() && now.referenced.count(it->first) > 0;
        const bool job_gone = !it->second.empty() && !now.live_jobs.count(it->second);
        it = (published || job_gone) ? reserved().erase(it) : std::next(it);
    }
    std::error_code ec;
    for (fs::directory_iterator it(dir(), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = utils::path_to_utf8(it->path().filename());
        if ((!is_valid_file_name(name) && !is_image_file_name(name)) ||
            now.referenced.count(name) || reserved().count(name)) continue;
        std::error_code rm;
        fs::remove(it->path(), rm);
        if (rm) LOG(WARNING, "Videos") << "Could not remove " << name << ": " << rm.message() << std::endl;
    }
}

std::string normalize_request(nlohmann::json& request) {
    if (request.contains("image") && !request["image"].is_null()) {
        if (!request["image"].is_string()) return "'image' must be a base64 string";
        if (const auto error = validate_image(request["image"].get_ref<const std::string&>()); !error.empty())
            return error;
        // TheNoise requires a non-blank prompt even for image-conditioned video.
        const auto prompt = request.find("prompt");
        if (prompt != request.end() && !prompt->is_null() && !prompt->is_string())
            return "'prompt' must be a string";
        if (prompt == request.end() || prompt->is_null() ||
            prompt->get_ref<const std::string&>().find_first_not_of(" \t\r\n") == std::string::npos)
            request["prompt"] = "Animate the image.";
    }
    struct IntegerField {
        const char* name;
        int64_t min;
        int64_t max;
    };
    const IntegerField integers[] = {
        {"video_frames", 1, kMaxVideoFrames},
        {"fps", 1, kMaxFps},
        {"width", 1, kMaxDimension},
        {"height", 1, kMaxDimension},
        {"steps", 1, kMaxSteps},
        {"seed", std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max()},
    };
    // 2^63 is exact as a double, while INT64_MAX is not: a double compared against
    // (double)INT64_MAX would let 2^63 itself through to an undefined cast.
    constexpr double kTwoPow63 = 9223372036854775808.0;
    for (const auto& field : integers) {
        if (!request.contains(field.name) || request[field.name].is_null()) continue;
        auto& value = request[field.name];
        const std::string out_of_range = std::string("'") + field.name + "' is out of range";
        if (!value.is_number()) return std::string("'") + field.name + "' must be an integer";
        int64_t number = 0;
        if (value.is_number_unsigned()) {
            const uint64_t unsigned_number = value.get<uint64_t>();
            if (unsigned_number > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                return out_of_range;
            number = static_cast<int64_t>(unsigned_number);
        } else if (value.is_number_integer()) {
            number = value.get<int64_t>();
        } else {
            const double floating = value.get<double>();
            if (!std::isfinite(floating) || std::floor(floating) != floating)
                return std::string("'") + field.name + "' must be an integer";
            if (floating < -kTwoPow63 || floating >= kTwoPow63) return out_of_range;
            number = static_cast<int64_t>(floating);
        }
        if (number < field.min || number > field.max) return out_of_range;
        value = number;
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

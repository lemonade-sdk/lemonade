#include "test_state_isolation.h"

#include "lemon/utils/path_utils.h"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <system_error>

namespace fs = std::filesystem;

namespace lemon {
namespace test {
namespace {

fs::path claim_state_dir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    // create_directory() reports whether this process created the directory, so
    // a losing attempt retries instead of sharing a root with a test binary
    // ctest started in parallel.
    for (int attempt = 0; attempt < 64; ++attempt) {
        fs::path dir = fs::temp_directory_path() /
                       ("lemonade-test-" + std::to_string(stamp) + "-" +
                        std::to_string(attempt));
        std::error_code ec;
        if (fs::create_directory(dir, ec)) {
            return dir;
        }
        if (ec && !fs::exists(dir)) {
            throw std::runtime_error("could not create a test state dir: " + ec.message());
        }
    }
    throw std::runtime_error("could not claim a unique test state dir");
}

struct Isolation {
    fs::path dir;

    Isolation() : dir(claim_state_dir()) {
        utils::set_cache_dir(dir.string());
        utils::set_config_dir(dir.string());
    }

    ~Isolation() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

Isolation& isolation() {
    static Isolation instance;
    return instance;
}

// Constructed before main(), so a test with its own directory layout to exercise
// (test_config_dir_migration) overrides this simply by calling the setters
// itself, with no opt-out list to keep in sync.
const bool installed = (isolation(), true);

} // namespace

const fs::path& state_dir() {
    (void)installed;
    return isolation().dir;
}

fs::path make_scratch_dir(const std::string& prefix) {
    static std::atomic<unsigned> counter{0};
    fs::path dir = state_dir() / (prefix + "-" + std::to_string(counter++));
    fs::create_directories(dir);
    return dir;
}

} // namespace test
} // namespace lemon

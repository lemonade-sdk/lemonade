#include "lemon/backends/hf_cache_util.h"
#include "lemon/utils/audio_mime.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace hf_cache = lemon::backends::hf_cache;
int checks = 0;
int failures = 0;

void check(bool ok, const std::string& label) {
    ++checks;
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label.c_str());
}

class CacheFixture {
public:
    CacheFixture() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        base = fs::temp_directory_path() / ("lemonade-artifact-" + std::to_string(stamp));
        if (!fs::create_directory(base)) throw std::runtime_error("Cannot create cache fixture");
    }
    ~CacheFixture() {
        std::error_code error;
        fs::remove_all(base, error);
    }
    fs::path cache(const std::string& name) {
        auto root = base / fs::u8path(name);
        fs::create_directories(root / "snapshots");
        return root;
    }
    void ref(const fs::path& root, const std::string& contents) {
        fs::create_directories(root / "refs");
        std::ofstream output(root / "refs/main", std::ios::binary);
        output << contents;
        output.close();
        if (!output) throw std::runtime_error("Cannot write cache ref fixture");
    }
    fs::path base;
};

void check_snapshots() {
    CacheFixture fixture;
    {
        auto root = fixture.cache("referenced");
        fs::create_directory(root / "snapshots/pinned-123");
        fs::create_directory(root / "snapshots/newer-unreferenced");
        fixture.ref(root, "pinned-123\n");
        check(hf_cache::active_snapshot_or_cache_path(root.u8string()) == (root / "snapshots/pinned-123").u8string(),
              "existing refs/main selects pinned snapshot despite another snapshot");
    }
    {
        auto root = fixture.cache("missing-ref");
        check(hf_cache::active_snapshot_or_cache_path(root.u8string()) == root.u8string(), "missing refs/main returns original cache");
    }
    {
        auto root = fixture.cache("unreferenced");
        fs::create_directory(root / "snapshots/unreferenced-456");
        check(hf_cache::active_snapshot_or_cache_path(root.u8string()) == root.u8string(), "unreferenced snapshot is never discovered by scanning");
    }
    {
        auto root = fixture.cache("empty-ref");
        fixture.ref(root, "");
        check(hf_cache::active_snapshot_or_cache_path(root.u8string()) == root.u8string(), "empty ref returns original cache");
        fixture.ref(root, " \t\r\n");
        check(hf_cache::active_snapshot_or_cache_path(root.u8string()) == root.u8string(), "whitespace-only ref returns original cache");
    }
    {
        auto root = fixture.cache("padded-ref");
        fs::create_directory(root / "snapshots/padded-789");
        fs::create_directory(root / "snapshots/ignored-second-line");
        fixture.ref(root, " \tpadded-789 \t\r\nignored-second-line\n");
        check(hf_cache::active_snapshot_or_cache_path(root.u8string()) == (root / "snapshots/padded-789").u8string(),
              "ref trims surrounding whitespace and reads only first line");
    }
    {
        auto root = fixture.cache("absent-snapshot");
        fixture.ref(root, "missing-000\n");
        fs::create_directory(root / "snapshots/other-111");
        check(hf_cache::active_snapshot_or_cache_path(root.u8string()) == root.u8string(), "absent referenced snapshot falls back even when another exists");
        const std::string original = root.u8string() + "/";
        check(hf_cache::active_snapshot_or_cache_path(original) == original, "fallback preserves original trailing separator byte for byte");
    }
    {
        auto root = fixture.cache(u8"模型-żółć");
        const auto snapshot = root / "snapshots" / fs::u8path(u8"wersja-音");
        fs::create_directory(snapshot);
        fixture.ref(root, u8"wersja-音\n");
        check(hf_cache::active_snapshot_or_cache_path(root.u8string()) == snapshot.u8string(), "Unicode cache and snapshot resolve as UTF-8");
        fixture.ref(root, "missing\n");
        const auto original = root.u8string() + "/";
        check(hf_cache::active_snapshot_or_cache_path(original) == original, "Unicode fallback preserves input bytes and trailing separator");
    }
    {
        auto root = fixture.cache("linked-snapshot");
        auto target = fixture.base / "snapshot-target";
        fs::create_directory(target);
        fixture.ref(root, "linked\n");
        std::error_code error;
        fs::create_directory_symlink(target, root / "snapshots/linked", error);
#ifdef _WIN32
        if (error) {
            std::printf("SKIP Windows HF symlink fixture: %s\n", error.message().c_str());
            return;
        }
#else
        if (error) throw std::runtime_error("Cannot create HF symlink fixture: " + error.message());
#endif
        check(hf_cache::exists(root / "snapshots/linked"), "HF existence mechanism accepts symlinked snapshot");
        check(hf_cache::active_snapshot_or_cache_path(root.u8string()) == (root / "snapshots/linked").u8string(),
              "symlinked snapshot returns lexical snapshot path without canonicalizing");
    }
}

void check_audio_mime() {
    const std::vector<std::pair<std::string_view, std::string>> cases = {
        {".mp3", "audio/mpeg"}, {".m4a", "audio/mp4"}, {".ogg", "audio/ogg"},
        {".flac", "audio/flac"}, {".webm", "audio/webm"}, {".wav", "audio/wav"},
        {"", "audio/wav"}, {".unknown", "audio/wav"}, {".MP3", "audio/wav"},
        {".FlAc", "audio/wav"}, {"mp3", "audio/wav"}, {".mp3 ", "audio/wav"},
    };
    for (const auto& [extension, mime] : cases) {
        check(lemon::utils::audio_mime_for_extension(extension) == mime, "literal case-sensitive MIME for '" + std::string(extension) + "'");
    }
    const std::string embedded = ".mp3tail";
    check(lemon::utils::audio_mime_for_extension(std::string_view(embedded.data(), 4)) == "audio/mpeg",
          "extension uses string_view length rather than null termination");
}

}

int main() {
    try {
        check_snapshots();
        check_audio_mime();
    } catch (const std::exception& error) {
        check(false, std::string("unexpected exception: ") + error.what());
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

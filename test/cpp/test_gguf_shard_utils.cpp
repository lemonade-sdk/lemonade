#include "lemon/gguf_shard_utils.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static fs::path make_file(const fs::path& dir, const std::string& name, std::uintmax_t bytes) {
    fs::path p = dir / name;
    { std::ofstream(p, std::ios::out | std::ios::trunc); }
    std::error_code ec;
    fs::resize_file(p, bytes, ec);
    if (ec) {
        std::printf("resize_file('%s') failed: %s\n", p.string().c_str(), ec.message().c_str());
    }
    return p;
}

static fs::path make_temp_test_dir() {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() / ("lemonade_gguf_shard_test_" + std::to_string(tick));
}

int main() {
    fs::path base_dir = make_temp_test_dir();
    std::error_code ec;
    fs::remove_all(base_dir, ec);
    fs::create_directories(base_dir, ec);

    {
        std::string b;
        bool ok = lemon::is_gguf_shard_filename("Laguna-S-2.1-UD-Q4_K_XL-00001-of-00003.gguf", &b);
        check("detects folder-sharded filename", ok);
        check("extracts correct shard base", b == "Laguna-S-2.1-UD-Q4_K_XL");
    }
    {
        std::string b;
        int t = 0;
        int i = 0;
        check("extracts total shard count",
              lemon::is_gguf_shard_filename("model-00001-of-00005.gguf", &b, &t, &i) && t == 5 && i == 1);
    }
    {
        std::string b;
        check("dot-separated shard is detected",
              lemon::is_gguf_shard_filename("model.00001-of-00003.gguf", &b) && b == "model");
    }
    {
        std::string b;
        check("underscore-separated shard is detected",
              lemon::is_gguf_shard_filename("model_00001-of-00003.gguf", &b) && b == "model");
    }
    {
        std::string b;
        check("bare numbered file is not a shard (no -of-)",
              !lemon::is_gguf_shard_filename("model-7.gguf", &b));
    }
    {
        std::string b;
        check("shard index without a separator is not a shard",
              !lemon::is_gguf_shard_filename("model00001-of-00003.gguf", &b));
    }
    {
        std::string b;
        check("plain model.gguf is not a shard", !lemon::is_gguf_shard_filename("model.gguf", &b));
    }
    {
        std::string b;
        check("interrupted .gguf.partial is not a shard",
              !lemon::is_gguf_shard_filename("model-00001-of-00003.gguf.partial", &b));
    }
    {
        std::string b;
        check("zero total is not a shard",
              !lemon::is_gguf_shard_filename("model-00001-of-00000.gguf", &b));
    }
    {
        std::string b;
        check("zero index is not a shard",
              !lemon::is_gguf_shard_filename("model-00000-of-00003.gguf", &b));
    }
    {
        std::string b;
        check("index greater than total is not a shard",
              !lemon::is_gguf_shard_filename("model-00004-of-00003.gguf", &b));
    }

    {
        check("matching base and total are the same family",
              lemon::same_shard_family("model-00002-of-00003.gguf", "model", 3));
        check("same base, different total is a different family",
              !lemon::same_shard_family("model-00001-of-00005.gguf", "model", 3));
        check("different base is a different family",
              !lemon::same_shard_family("other-00001-of-00003.gguf", "model", 3));
        check("non-shard filename is not a family member",
              !lemon::same_shard_family("model.gguf", "model", 3));
        check("out-of-range index is not a family member",
              !lemon::same_shard_family("model-00004-of-00003.gguf", "model", 3));
    }

    {
        fs::path sub = base_dir / "same_dir";
        fs::create_directories(sub, ec);
        make_file(sub, "same-00001-of-00003.gguf", 10);
        make_file(sub, "same-00002-of-00003.gguf", 20);
        make_file(sub, "same-00003-of-00003.gguf", 30);
        make_file(sub, "same-00001-of-00005.gguf", 500);
        make_file(sub, "other-00001-of-00002.gguf", 1000);
        make_file(sub, "model.gguf", 9000);
        make_file(sub, "model-7.gguf", 8000);

        check("of-3 family sums only its own shards",
              lemon::sharded_gguf_size_bytes(sub / "same-00001-of-00003.gguf") == 10 + 20 + 30);
        check("of-5 family does not absorb the of-3 family",
              lemon::sharded_gguf_size_bytes(sub / "same-00001-of-00005.gguf") == 500);
        check("different-base family sums independently",
              lemon::sharded_gguf_size_bytes(sub / "other-00001-of-00002.gguf") == 1000);
        check("non-shard path aggregates nothing",
              lemon::sharded_gguf_size_bytes(sub / "model.gguf") == 0);
    }

    {
        fs::path sub = base_dir / "widths";
        fs::create_directories(sub, ec);
        make_file(sub, "Q4_K_M-1-of-2.gguf", 50);
        make_file(sub, "Q4_K_M-2-of-2.gguf", 60);
        check("varying index widths are the same family",
              lemon::sharded_gguf_size_bytes(sub / "Q4_K_M-1-of-2.gguf") == 50 + 60);
    }

    {
        fs::path sub = base_dir / "separators";
        fs::create_directories(sub, ec);
        make_file(sub, "model.00001-of-00002.gguf", 70);
        make_file(sub, "model.00002-of-00002.gguf", 80);
        make_file(sub, "other_00001-of-00002.gguf", 90);
        make_file(sub, "other_00002-of-00002.gguf", 100);
        check("dot-separated shards are summed",
              lemon::sharded_gguf_size_bytes(sub / "model.00001-of-00002.gguf") == 70 + 80);
        check("underscore-separated shards are summed",
              lemon::sharded_gguf_size_bytes(sub / "other_00001-of-00002.gguf") == 90 + 100);
    }

    fs::remove_all(base_dir, ec);

    if (g_failures == 0) {
        std::printf("\nAll gguf_shard tests passed\n");
        return 0;
    }
    std::printf("\n%d gguf_shard test(s) FAILED\n", g_failures);
    return 1;
}

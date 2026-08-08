// Standalone test for lemon shard-size aggregation
// (src/cpp/include/lemon/gguf_shard_utils.h).
//
// Covers the regression from #2972: a folder-sharded GGUF variant must report
// the sum of all its sibling shards (not just the first pointer-sized one),
// while unrelated GGUF files in the same directory and bare numbered files
// (model-7.gguf) must not be merged into a shard family.
//
// Compile with: cl /std:c++17 /EHsc /I src/cpp/include test/cpp/test_gguf_shard_files.cpp
// or:          g++ -std=c++17 -I src/cpp/include test/cpp/test_gguf_shard_files.cpp -o gguf_shard_test

#include "lemon/gguf_shard_utils.h"

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

int main() {
    fs::path base_dir = fs::temp_directory_path() / "lemonade_gguf_shard_test";
    std::error_code ec;
    fs::remove_all(base_dir, ec);
    fs::create_directories(base_dir, ec);

    // --- is_gguf_shard_filename ------------------------------------------------
    {
        std::string b;
        bool ok = lemon::is_gguf_shard_filename("Laguna-S-2.1-UD-Q4_K_XL-00001-of-00003.gguf", &b);
        check("detects folder-sharded filename", ok);
        check("extracts correct shard base", b == "Laguna-S-2.1-UD-Q4_K_XL");
    }
    {
        std::string b;
        check("bare numbered file is not a shard (no -of-)",
              !lemon::is_gguf_shard_filename("model-7.gguf", &b));
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

    // --- sharded sum: all sibling shards of one family --------------------------
    {
        make_file(base_dir, "model-00001-of-00003.gguf", 100);
        make_file(base_dir, "model-00002-of-00003.gguf", 200);
        make_file(base_dir, "model-00003-of-00003.gguf", 300);

        std::uintmax_t from_first = lemon::sharded_gguf_size_bytes(base_dir / "model-00001-of-00003.gguf");
        std::uintmax_t from_second = lemon::sharded_gguf_size_bytes(base_dir / "model-00002-of-00003.gguf");
        check("sums all sibling shards from the first shard", from_first == 100 + 200 + 300);
        check("sums the same total regardless of entry shard", from_second == from_first);
    }

    // --- unrelated GGUF files are not summed into a shard family ----------------
    {
        make_file(base_dir, "other-00001-of-00002.gguf", 1000);  // different base
        make_file(base_dir, "model.gguf", 9000);                  // non-shard
        make_file(base_dir, "model-7.gguf", 8000);               // bare numbered, not a shard

        std::uintmax_t family_total = lemon::sharded_gguf_size_bytes(base_dir / "model-00001-of-00003.gguf");
        check("unrelated GGUF files are not summed into the shard family",
              family_total == 100 + 200 + 300);
        check("non-shard GGUF path has 0 aggregate",
              lemon::sharded_gguf_size_bytes(base_dir / "model.gguf") == 0);
        check("bare numbered GGUF path has 0 aggregate",
              lemon::sharded_gguf_size_bytes(base_dir / "model-7.gguf") == 0);
        check("different-base shard family is summed independently",
              lemon::sharded_gguf_size_bytes(base_dir / "other-00001-of-00002.gguf") == 1000);
    }

    // --- varying shard-index widths merge into one family -----------------------
    {
        fs::path sub = base_dir / "widths";
        fs::create_directories(sub, ec);
        make_file(sub, "Q4_K_M-1-of-2.gguf", 50);
        make_file(sub, "Q4_K_M-2-of-2.gguf", 60);
        check("varying index widths merge into one family",
              lemon::sharded_gguf_size_bytes(sub / "Q4_K_M-1-of-2.gguf") == 50 + 60);
    }

    // --- a path that is not a shard (and not even a .gguf) returns 0 ------------
    {
        check("single whole-file model.gguf returns 0 aggregate",
              lemon::sharded_gguf_size_bytes(make_file(base_dir, "whole.gguf", 42)) == 0);
    }

    fs::remove_all(base_dir, ec);

    if (g_failures == 0) {
        std::printf("\nAll gguf_shard tests passed\n");
        return 0;
    }
    std::printf("\n%d gguf_shard test(s) FAILED\n", g_failures);
    return 1;
}

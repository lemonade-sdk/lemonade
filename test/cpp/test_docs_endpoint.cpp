// Standalone test for lemon::load_api_docs() (issue #1700).
// Compile with: cl /std:c++17 /EHsc /I src/cpp/include test/cpp/test_docs_endpoint.cpp
//              src/cpp/server/api_docs.cpp
// or:          g++ -std=c++17 -I src/cpp/include test/cpp/test_docs_endpoint.cpp
//              src/cpp/server/api_docs.cpp -lpthread -o docs_endpoint_test

#include "lemon/api_docs.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path make_scratch_dir(const std::string& name) {
    fs::path dir = fs::temp_directory_path() / ("lemon_docs_test_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void write_file(const fs::path& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    f << content;
}

} // namespace

int main() {
    int failures = 0;

    // Case 1 + 2: all three files present, in order, version header correct.
    {
        fs::path dir = make_scratch_dir("all_present");
        write_file(dir / "lemonade.md", "LEMONADE_CONTENT");
        write_file(dir / "llamacpp.md", "LLAMACPP_CONTENT");
        write_file(dir / "mcp.md", "MCP_CONTENT");

        std::string result = lemon::load_api_docs(dir.string(), "9.9.9");

        auto lemonade_pos = result.find("LEMONADE_CONTENT");
        auto llamacpp_pos = result.find("LLAMACPP_CONTENT");
        auto mcp_pos = result.find("MCP_CONTENT");

        bool header_ok = result.find("# Lemonade Server API Reference (9.9.9)") == 0;
        bool all_present = lemonade_pos != std::string::npos &&
                            llamacpp_pos != std::string::npos &&
                            mcp_pos != std::string::npos;
        bool order_ok = all_present && lemonade_pos < llamacpp_pos && llamacpp_pos < mcp_pos;

        bool ok = header_ok && all_present && order_ok;
        std::printf("[%s] all files present, version header, correct order\n", ok ? "PASS" : "FAIL");
        if (!ok) ++failures;
    }

    // Case 3: one file missing does not throw, and the rest still appear.
    {
        fs::path dir = make_scratch_dir("one_missing");
        write_file(dir / "lemonade.md", "LEMONADE_CONTENT");
        write_file(dir / "mcp.md", "MCP_CONTENT");
        // llamacpp.md intentionally absent.

        std::string result;
        bool threw = false;
        try {
            result = lemon::load_api_docs(dir.string(), "1.0.0");
        } catch (...) {
            threw = true;
        }

        bool ok = !threw &&
                  result.find("LEMONADE_CONTENT") != std::string::npos &&
                  result.find("MCP_CONTENT") != std::string::npos;
        std::printf("[%s] one file missing: no throw, remaining files present\n", ok ? "PASS" : "FAIL");
        if (!ok) ++failures;
    }

    // Case 4: directory missing entirely fails cleanly, no throw.
    {
        fs::path dir = fs::temp_directory_path() / "lemon_docs_test_does_not_exist";
        std::error_code ec;
        fs::remove_all(dir, ec);

        std::string result;
        bool threw = false;
        try {
            result = lemon::load_api_docs(dir.string(), "1.0.0");
        } catch (...) {
            threw = true;
        }

        bool ok = !threw && result.find("# Lemonade Server API Reference (1.0.0)") == 0;
        std::printf("[%s] missing directory: no throw, header still returned\n", ok ? "PASS" : "FAIL");
        if (!ok) ++failures;
    }

    // Case 5: an empty file is handled without crashing.
    {
        fs::path dir = make_scratch_dir("empty_file");
        write_file(dir / "lemonade.md", "");
        write_file(dir / "llamacpp.md", "LLAMACPP_CONTENT");
        write_file(dir / "mcp.md", "MCP_CONTENT");

        std::string result;
        bool threw = false;
        try {
            result = lemon::load_api_docs(dir.string(), "1.0.0");
        } catch (...) {
            threw = true;
        }

        bool ok = !threw && result.find("LLAMACPP_CONTENT") != std::string::npos;
        std::printf("[%s] empty file: no crash, other files still present\n", ok ? "PASS" : "FAIL");
        if (!ok) ++failures;
    }

    std::printf("\n%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}

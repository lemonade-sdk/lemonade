#include "lemon/api_docs.h"

#include <lemon/utils/aixlog.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace lemon {

namespace {

// Order matches CMakeLists.txt's API_DOC_FILES list.
constexpr const char* kApiDocFiles[] = {"lemonade.md", "llamacpp.md", "mcp.md"};

} // namespace

std::string load_api_docs(const std::string& docs_dir, const std::string& version) {
    std::ostringstream out;
    out << "# Lemonade Server API Reference (" << version << ")\n\n";

    std::error_code ec;
    if (!fs::exists(docs_dir, ec) || ec) {
        LOG(WARNING, "Server") << "API docs directory not found: " << docs_dir << std::endl;
        return out.str();
    }

    for (const char* filename : kApiDocFiles) {
        fs::path file_path = fs::path(docs_dir) / filename;
        std::ifstream file(file_path, std::ios::binary);
        if (!file) {
            LOG(WARNING, "Server") << "API doc file missing: " << file_path.string() << std::endl;
            continue;
        }
        // Read into a throwaway stream first: operator<<(ostream&, streambuf*)
        // sets failbit on the destination when it extracts zero bytes, which
        // would silently poison `out` (and drop every file appended after
        // it) the first time an empty file is encountered.
        std::ostringstream content;
        content << file.rdbuf();
        out << content.str() << "\n\n";
    }

    return out.str();
}

} // namespace lemon

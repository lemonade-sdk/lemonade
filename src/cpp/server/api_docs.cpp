#include "lemon/api_docs.h"

#include <lemon/utils/aixlog.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace lemon {

namespace {

std::string doc_id_from_path(const fs::path& relative) {
    fs::path stem = relative.parent_path() / relative.stem();
    return stem.generic_string();
}

// Documents are titled by their leading H1 so the index cannot drift from the file.
std::string read_title(const fs::path& file_path, const std::string& fallback) {
    std::ifstream file(file_path);
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("# ", 0) == 0) {
            std::string title = line.substr(2);
            while (!title.empty() && (title.back() == '\r' || title.back() == ' ')) {
                title.pop_back();
            }
            if (!title.empty()) {
                return title;
            }
        }
    }
    return fallback;
}

// Resolves id under docs_dir, rejecting anything that escapes it. Mirrors the
// confinement checks used for web-app assets in server.cpp.
bool resolve_doc_path(const fs::path& base, const std::string& id, fs::path& resolved) {
    if (id.empty()) {
        return false;
    }

    std::string relative = id;
    const std::string suffix = ".md";
    if (relative.size() > suffix.size() &&
        relative.compare(relative.size() - suffix.size(), suffix.size(), suffix) == 0) {
        relative.erase(relative.size() - suffix.size());
    }

    std::error_code ec;
    fs::path candidate = fs::weakly_canonical(base / (relative + suffix), ec);
    if (ec) {
        return false;
    }

    fs::path within = fs::relative(candidate, base, ec);
    if (ec || within.empty() || within.is_absolute()) {
        return false;
    }
    for (const auto& part : within) {
        if (part == "..") {
            return false;
        }
    }

    resolved = candidate;
    return true;
}

} // namespace

std::vector<ApiDoc> list_api_docs(const std::string& docs_dir) {
    std::vector<ApiDoc> docs;

    std::error_code ec;
    fs::path base = fs::weakly_canonical(fs::path(docs_dir), ec);
    if (ec || !fs::is_directory(base, ec)) {
        LOG(WARNING, "Server") << "API docs directory not found: " << docs_dir << std::endl;
        return docs;
    }

    for (fs::recursive_directory_iterator it(base, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        if (!it->is_regular_file(ec) || it->path().extension() != ".md") {
            continue;
        }

        fs::path relative = fs::relative(it->path(), base, ec);
        if (ec) {
            continue;
        }

        ApiDoc doc;
        doc.id = doc_id_from_path(relative);
        doc.title = read_title(it->path(), doc.id);
        doc.bytes = fs::file_size(it->path(), ec);
        if (ec) {
            doc.bytes = 0;
        }
        docs.push_back(std::move(doc));
    }

    std::sort(docs.begin(), docs.end(),
              [](const ApiDoc& a, const ApiDoc& b) { return a.id < b.id; });
    return docs;
}

bool read_api_doc(const std::string& docs_dir, const std::string& id, std::string& content) {
    std::error_code ec;
    fs::path base = fs::weakly_canonical(fs::path(docs_dir), ec);
    if (ec) {
        return false;
    }

    fs::path resolved;
    if (!resolve_doc_path(base, id, resolved)) {
        return false;
    }

    std::ifstream file(resolved, std::ios::binary);
    if (!file) {
        return false;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    content = buffer.str();
    return true;
}

} // namespace lemon

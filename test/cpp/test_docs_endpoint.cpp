#include "lemon/api_docs.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++failures;
    }
}

fs::path make_scratch_dir(const std::string& name) {
    fs::path dir = fs::temp_directory_path() / ("lemon_docs_test_" + name);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "api");
    return dir;
}

void write_file(const fs::path& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    f << content;
}

const lemon::ApiDoc* find_doc(const std::vector<lemon::ApiDoc>& docs, const std::string& id) {
    for (const auto& doc : docs) {
        if (doc.id == id) {
            return &doc;
        }
    }
    return nullptr;
}

} // namespace

int main() {
    {
        fs::path dir = make_scratch_dir("index");
        write_file(dir / "api" / "lemonade.md", "# Lemonade API\n\nbody\n");
        write_file(dir / "api" / "openai.md", "# OpenAI Compatible\n");
        write_file(dir / "api" / "untitled.md", "no heading here\n");
        write_file(dir / "api" / "notes.txt", "ignored");

        auto docs = lemon::list_api_docs(dir.string());

        check(docs.size() == 3, "only .md files are indexed");

        const auto* lemonade = find_doc(docs, "api/lemonade");
        check(lemonade != nullptr, "ids are website-relative paths without the extension");
        check(lemonade != nullptr && lemonade->title == "Lemonade API", "title comes from the leading H1");
        check(lemonade != nullptr && lemonade->bytes == fs::file_size(dir / "api" / "lemonade.md"),
              "bytes reports the file size");

        const auto* untitled = find_doc(docs, "api/untitled");
        check(untitled != nullptr && untitled->title == "api/untitled",
              "title falls back to the id when there is no H1");

        bool sorted = true;
        for (std::size_t i = 1; i < docs.size(); ++i) {
            if (!(docs[i - 1].id < docs[i].id)) {
                sorted = false;
            }
        }
        check(sorted, "index is sorted by id");
    }

    {
        fs::path dir = fs::temp_directory_path() / "lemon_docs_test_absent";
        std::error_code ec;
        fs::remove_all(dir, ec);
        check(lemon::list_api_docs(dir.string()).empty(), "missing docs directory yields an empty index");
    }

    {
        fs::path dir = make_scratch_dir("read");
        write_file(dir / "api" / "lemonade.md", "# Lemonade API\ncontent\n");
        write_file(dir.parent_path() / "lemon_docs_test_read_outside.md", "SECRET");

        std::string content;
        check(lemon::read_api_doc(dir.string(), "api/lemonade", content) &&
                  content.find("content") != std::string::npos,
              "document is readable by id");

        content.clear();
        check(lemon::read_api_doc(dir.string(), "api/lemonade.md", content) &&
                  content.find("content") != std::string::npos,
              "the .md suffix is accepted too");

        check(!lemon::read_api_doc(dir.string(), "api/missing", content),
              "unknown id is rejected");
        check(!lemon::read_api_doc(dir.string(), "../lemon_docs_test_read_outside", content),
              "traversal outside the docs directory is rejected");
        check(!lemon::read_api_doc(dir.string(), "api/../../lemon_docs_test_read_outside", content),
              "embedded traversal is rejected");
        check(!lemon::read_api_doc(dir.string(), "", content), "empty id is rejected");
    }

    std::printf("%s\n", failures == 0 ? "All docs endpoint tests passed" : "Docs endpoint tests FAILED");
    return failures == 0 ? 0 : 1;
}

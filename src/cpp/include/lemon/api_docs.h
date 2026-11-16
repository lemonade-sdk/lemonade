#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lemon {

struct ApiDoc {
    std::string id;
    std::string title;
    std::uintmax_t bytes = 0;
};

// Documents bundled with the server, sorted by id. Empty when docs_dir is absent.
std::vector<ApiDoc> list_api_docs(const std::string& docs_dir);

// Reads one document by id, with or without the ".md" suffix. Returns false when the
// id names no bundled document or resolves outside docs_dir.
bool read_api_doc(const std::string& docs_dir, const std::string& id, std::string& content);

} // namespace lemon

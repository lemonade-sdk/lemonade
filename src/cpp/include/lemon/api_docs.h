#pragma once

#include <string>

namespace lemon {

// Returns the available API docs in package order, prefixed by the server version.
std::string load_api_docs(const std::string& docs_dir, const std::string& version);

} // namespace lemon

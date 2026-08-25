#pragma once

#include <string>

namespace lemon {

// Concatenates the Lemonade-specific API reference files staged into
// docs_dir (resources/api-docs/, copied from docs/api/ at build time) into a
// single markdown document, prefixed with the running server version.
//
// Missing or unreadable files are skipped rather than treated as fatal: the
// goal is to answer "what can this server do" from whatever is actually on
// disk, not to guarantee every file is present.
std::string load_api_docs(const std::string& docs_dir, const std::string& version);

} // namespace lemon

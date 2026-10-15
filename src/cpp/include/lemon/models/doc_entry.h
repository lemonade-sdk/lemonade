#pragma once

#include <string>
#include <vector>

namespace lemon {
namespace models {

// What a source says about itself. The generator turns these into docs; a fact
// no page renders stays out.
struct DocEntry {
    std::string id;
    std::string title;
    std::string summary;
    std::vector<std::string> notes;
    std::string example;  // rendered as one line, `[]` marks a placeholder
};

struct EnvVar {
    std::string name;
    std::string purpose;
    bool required = false;
};

}  // namespace models
}  // namespace lemon

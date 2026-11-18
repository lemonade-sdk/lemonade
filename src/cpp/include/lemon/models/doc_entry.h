#pragma once

#include <string>
#include <vector>

namespace lemon {
namespace models {

struct DocEntry {
    std::string id;
    std::string title;
    std::string summary;
    std::vector<std::string> notes;
    std::string example;
};

struct EnvVar {
    std::string name;
    std::string purpose;
    bool required = false;
};

}  // namespace models
}  // namespace lemon

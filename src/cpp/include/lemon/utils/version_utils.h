#pragma once

#include <string>
#include <vector>

namespace lemon {
namespace utils {

// Semantic version parser and comparator
// Handles versions like "1.2.3", "v1.2.3", "32.0.203.311"
class Version {
public:
    static Version parse(const std::string& version_str);

    bool operator>=(const Version& other) const;
    bool operator==(const Version& other) const;
    std::string to_string() const;
    bool empty() const { return parts_.empty(); }

private:
    std::vector<int> parts_;
    explicit Version(const std::vector<int>& parts) : parts_(parts) {}
};

} // namespace utils
} // namespace lemon

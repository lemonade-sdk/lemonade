#include <lemon/utils/version_utils.h>
#include <sstream>
#include <cctype>

namespace lemon {
namespace utils {

Version Version::parse(const std::string& version_str) {
    if (version_str.empty()) {
        return Version({});
    }

    std::vector<int> parts;
    std::string working = version_str;

    // Remove 'v' or 'V' prefix if present
    if (!working.empty() && (working[0] == 'v' || working[0] == 'V')) {
        working = working.substr(1);
    }

    // Parse numeric parts separated by '.'
    std::stringstream ss(working);
    std::string part;
    while (std::getline(ss, part, '.')) {
        // Extract only numeric portion of each part
        std::string numeric;
        for (char c : part) {
            if (std::isdigit(static_cast<unsigned char>(c))) {
                numeric += c;
            } else {
                break; // Stop at first non-digit
            }
        }

        if (!numeric.empty()) {
            try {
                parts.push_back(std::stoi(numeric));
            } catch (...) {
                parts.push_back(0);
            }
        }
    }

    return Version(parts);
}

bool Version::operator>=(const Version& other) const {
    if (parts_.empty() || other.parts_.empty()) {
        return false;
    }

    // Compare element by element
    size_t max_len = std::max(parts_.size(), other.parts_.size());
    for (size_t i = 0; i < max_len; ++i) {
        int this_part = (i < parts_.size()) ? parts_[i] : 0;
        int other_part = (i < other.parts_.size()) ? other.parts_[i] : 0;

        if (this_part > other_part) {
            return true;
        }
        if (this_part < other_part) {
            return false;
        }
    }

    return true; // Equal versions
}

bool Version::operator==(const Version& other) const {
    if (parts_.size() != other.parts_.size()) {
        // Pad to same length for comparison
        size_t max_len = std::max(parts_.size(), other.parts_.size());
        for (size_t i = 0; i < max_len; ++i) {
            int this_part = (i < parts_.size()) ? parts_[i] : 0;
            int other_part = (i < other.parts_.size()) ? other.parts_[i] : 0;
            if (this_part != other_part) {
                return false;
            }
        }
        return true;
    }

    return parts_ == other.parts_;
}

std::string Version::to_string() const {
    if (parts_.empty()) {
        return "";
    }

    std::string result = std::to_string(parts_[0]);
    for (size_t i = 1; i < parts_.size(); ++i) {
        result += "." + std::to_string(parts_[i]);
    }
    return result;
}

} // namespace utils
} // namespace lemon

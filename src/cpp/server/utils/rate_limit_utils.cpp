#include <lemon/utils/rate_limit_utils.h>

#include <cctype>
#include <cstdint>
#include <string>

namespace lemon::utils {

int64_t parse_rate_limit_to_bytes(const std::string& raw) {
    size_t start = raw.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return 0;
    size_t end = raw.find_last_not_of(" \t\r\n");
    const std::string s = raw.substr(start, end - start + 1);
    if (s.empty()) return 0;

    size_t i = 0;
    uint64_t value = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
        if (value > (INT64_MAX - (uint64_t)(s[i] - '0')) / 10) return -1;
        value = value * 10 + (uint64_t)(s[i] - '0');
    }
    if (i == 0) return -1;  // Must start with a digit (no sign or exponent).

    uint64_t prec = 0;
    size_t plen = 0;
    if (i < s.size() && s[i] == '.') {
        ++i;
        const size_t dstart = i;
        for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
            prec = prec * 10 + (uint64_t)(s[i] - '0');
            ++plen;
            if (prec > INT64_MAX) return -1;
        }
        if (i == dstart) return -1;  // "." with no following digits.
    }

    std::string suffix = s.substr(i);
    if (!suffix.empty() && (suffix.back() == 'b' || suffix.back() == 'B')) {
        suffix.pop_back();
    }
    if (!suffix.empty() && suffix.size() != 1) return -1;  // Single unit character only.

    uint64_t mul = 1;
    size_t mlen = 0;  // Decimal digits in mul (for fractional precision trimming).
    if (!suffix.empty()) {
        switch (static_cast<char>(std::toupper(static_cast<unsigned char>(suffix[0])))) {
        case 'K': mul = 1024ULL; mlen = 4; break;
        case 'M': mul = 1024ULL * 1024ULL; mlen = 7; break;
        case 'G': mul = 1024ULL * 1024ULL * 1024ULL; mlen = 10; break;
        case 'T': mul = 1024ULL * 1024ULL * 1024ULL * 1024ULL; mlen = 13; break;
        case 'P': mul = 1024ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL; mlen = 16; break;
        default: return -1;
        }
    }

    // A fractional part without a unit is ambiguous; reject it like curl.
    if (plen > 0 && mul == 1) return -1;

    uint64_t add = 0;
    if (prec) {
        while (plen > mlen) {  // Trim precision digits beyond the unit's resolution.
            prec /= 10;
            --plen;
        }
        uint64_t frac = 1;
        for (size_t k = 0; k < plen; ++k) frac *= 10;
        if (prec != 0) {
            if (INT64_MAX / mul >= prec) {
                add = mul * prec / frac;
            } else {
                add = (mul / frac) * prec;
            }
        }
    }

    if (value > (static_cast<uint64_t>(INT64_MAX) - add) / mul) return -1;
    const uint64_t bytes = value * mul + add;
    if (bytes > static_cast<uint64_t>(INT64_MAX)) return -1;
    return static_cast<int64_t>(bytes);
}

} // namespace lemon::utils

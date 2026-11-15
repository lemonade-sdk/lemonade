#pragma once

#include <string>
#include <variant>

namespace lnsdk {

/// Generic result type for all SDK operations.
template <typename T>
struct Result {
    bool        ok{false};
    T           value{};
    int         http_status{0};
    std::string error;

    /// Convenience: implicit bool conversion.
    explicit operator bool() const noexcept { return ok; }
};

/// Result for operations that return no value.
using StatusResult = Result<std::monostate>;

} // namespace lnsdk

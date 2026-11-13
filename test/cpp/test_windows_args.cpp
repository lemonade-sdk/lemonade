#include <lemon/utils/windows_args.h>

#include "windows_argv_cases.h"

#include <iomanip>
#include <iostream>

int main() {
    int passed = 0;
    int failed = 0;
    for (const auto& test : windows_arg_cases) {
        const auto actual = lemon::utils::escape_windows_arg(test.input);
        const bool matches = actual == test.escaped;
        std::cout << (matches ? "PASS " : "FAIL ") << test.name << '\n';
        if (matches) {
            ++passed;
        } else {
            ++failed;
            std::cout << "  expected: " << std::quoted(test.escaped)
                      << "\n  actual:   " << std::quoted(actual) << '\n';
        }
    }
    std::cout << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}

#include <lemon/utils/windows_args.h>

namespace lemon::utils {

std::string escape_windows_arg(const std::string& arg) {
    std::string result = "\"";
    size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        result.append(c == '"' ? backslashes * 2 + 1 : backslashes, '\\');
        result += c;
        backslashes = 0;
    }
    result.append(backslashes * 2, '\\');
    result += '"';
    return result;
}

}

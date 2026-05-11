// Standalone regression tests for lemon::RecipeOptions.
// Compile with:
//   g++ -std=c++17 -I src/cpp/include src/cpp/server/recipe_options.cpp test/cpp/test_recipe_options.cpp -o /tmp/test_recipe_options

#include "lemon/recipe_options.h"
#include "lemon/system_info.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

namespace lemon {

SystemInfo::SupportedBackendsResult SystemInfo::get_supported_backends(const std::string& recipe) {
    if (recipe == "lemon-mlx") {
        return {{"cpu"}, ""};
    }
    return {{}, ""};
}

} // namespace lemon

using lemon::RecipeOptions;
using lemon::json;

static bool json_string_eq(const json& value, const char* expected) {
    return value.is_string() && value.get<std::string>() == expected;
}

int main() {
    int failures = 0;

    {
        RecipeOptions options(
            "lemon-mlx",
            json{{"lemon-mlx_backend", "rocm"}, {"lemon-mlx_args", "--draft"}}
        );
        const json serialized = options.to_json();
        const bool ok =
            serialized.contains("lemon-mlx_backend") &&
            serialized.contains("lemon-mlx_args") &&
            json_string_eq(options.get_option("lemon-mlx_backend"), "rocm") &&
            json_string_eq(options.get_option("lemon-mlx_args"), "--draft");
        std::printf("[%s] lemon-mlx preserves explicit backend and args\n", ok ? "PASS" : "FAIL");
        if (!ok) ++failures;
    }

    {
        RecipeOptions options("lemon-mlx", json::object());
        const bool ok =
            json_string_eq(options.get_option("lemon-mlx_backend"), "cpu") &&
            json_string_eq(options.get_option("lemon-mlx_args"), "");
        std::printf("[%s] lemon-mlx default options are strings\n", ok ? "PASS" : "FAIL");
        if (!ok) ++failures;
    }

    {
        const std::vector<std::string> cli = RecipeOptions::to_cli_options(
            json{{"lemon-mlx_backend", "cpu"}, {"lemon-mlx_args", "--ctx-size 2048"}}
        );
        const std::vector<std::string> expected = {
            "--lemon-mlx-args",
            "--ctx-size 2048",
            "--lemon-mlx",
            "cpu",
        };
        const bool ok = cli == expected;
        std::printf("[%s] lemon-mlx options map to CLI flags\n", ok ? "PASS" : "FAIL");
        if (!ok) ++failures;
    }

    return failures == 0 ? 0 : 1;
}

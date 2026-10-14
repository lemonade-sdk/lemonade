#include "lemon/backends/backend_load_helpers.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/error_types.h"
#include "lemon/recipe_options.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace lemon;
using namespace lemon::backends;
using Environment = std::vector<std::pair<std::string, std::string>>;
int checks = 0;
int failures = 0;

void check(bool ok, const std::string& label) {
    ++checks;
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label.c_str());
}

struct SentinelException : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct BinaryFixture {
    BackendSpec spec{"fixture-recipe", "fixture-server"};
    std::string backend = "rocm-stable";
    std::string external = "external path/server";
    bool external_exists = true;
    std::string failing_stage;
    std::exception_ptr failure = std::make_exception_ptr(SentinelException("sentinel failure"));
    std::vector<std::string> events;
    std::vector<std::pair<std::string, std::string>> arguments;
    std::vector<std::string> existence_paths;
    const BackendSpec* managed_spec = nullptr;

    void event(const std::string& stage) {
        events.push_back(stage);
        if (stage == failing_stage) std::rethrow_exception(failure);
    }

    BackendBinaryServices services() {
        return {
            [this](const std::string& recipe, const std::string& selected) {
                arguments.emplace_back(recipe, selected);
                event("find_external");
                return external;
            },
            [this](const std::string& path) {
                existence_paths.push_back(path);
                event("exists");
                return external_exists;
            },
            [this](const std::string& recipe, const std::string& selected) {
                arguments.emplace_back(recipe, selected);
                event("install");
            },
            [this](const BackendSpec& selected_spec, const std::string& selected) {
                managed_spec = &selected_spec;
                arguments.emplace_back(selected_spec.recipe, selected);
                event("managed_path");
                return std::string("managed path/server");
            },
        };
    }

    std::string resolve() {
        return resolve_external_or_managed_binary(spec, backend, services());
    }
};

void check_binary_resolution() {
    {
        BinaryFixture fixture;
        check(fixture.resolve() == "external path/server", "external path returned unchanged");
        check(fixture.events == std::vector<std::string>{"find_external", "exists"}, "external hit trace excludes install and managed lookup");
        check(fixture.existence_paths == std::vector<std::string>{"external path/server"}, "exists receives exact external string");
        check(fixture.arguments == std::vector<std::pair<std::string, std::string>>{{"fixture-recipe", "rocm-stable"}}, "external recipe and backend pass through unchanged");
    }
    {
        BinaryFixture fixture;
        fixture.external.clear();
        check(fixture.resolve() == "managed path/server", "empty override returns managed path");
        check(fixture.events == std::vector<std::string>{"find_external", "install", "managed_path"}, "empty override skips exists and installs before lookup");
        check(fixture.managed_spec == &fixture.spec, "managed lookup receives original spec object");
        check(fixture.arguments == std::vector<std::pair<std::string, std::string>>{
            {"fixture-recipe", "rocm-stable"}, {"fixture-recipe", "rocm-stable"}, {"fixture-recipe", "rocm-stable"}},
            "all services receive exact recipe and rocm-stable strings");
    }
    {
        BinaryFixture fixture;
        fixture.external_exists = false;
        check(fixture.resolve() == "managed path/server", "absent override returns managed path");
        check(fixture.events == std::vector<std::string>{"find_external", "exists", "install", "managed_path"}, "absent override full ordered trace");
    }
    const std::vector<std::pair<std::string, std::vector<std::string>>> exceptions = {
        {"find_external", {"find_external"}},
        {"exists", {"find_external", "exists"}},
        {"install", {"find_external", "exists", "install"}},
        {"managed_path", {"find_external", "exists", "install", "managed_path"}},
    };
    for (const auto& [stage, expected] : exceptions) {
        BinaryFixture fixture;
        fixture.external_exists = false;
        fixture.failing_stage = stage;
        bool exact_exception = false;
        try {
            fixture.resolve();
        } catch (const SentinelException& error) {
            exact_exception = std::current_exception() == fixture.failure && std::string(error.what()) == "sentinel failure";
        }
        check(exact_exception, stage + " propagates original exception object and type");
        check(fixture.events == expected, stage + " exception prevents every later callback");
    }
}

struct WrapperFixture {
    RecipeOptions options;
    BinaryFixture binary;
    int supported_queries = 0;

    std::string prepare(bool /*do_not_upgrade*/) {
        std::string backend = options.get_option("thinksound_backend");
        if (backend.empty()) {
            ++supported_queries;
            backend = first_supported_backend({"cuda", "vulkan", "rocm"}, "fixture operation", "fixture target");
        }
        binary.backend = backend;
        return binary.resolve();
    }
};

void check_selection() {
    check(first_supported_backend({"cuda", "vulkan", "rocm"}, "unused", "unused") == "cuda", "selection preserves declared order");
    check(first_supported_backend({"rocm", "cuda", "vulkan"}, "unused", "unused") == "rocm", "reordering changes first selection");
    check(first_supported_backend({"rocm-stable", "rocm-stable", "cuda"}, "unused", "unused") == "rocm-stable", "selection does not normalize channel or duplicates");
    check(first_supported_backend({"", "cuda"}, "unused", "unused").empty(), "selection does not validate first element");
    struct ErrorCase {
        std::string operation;
        std::string target;
        std::string message;
    };
    const std::vector<ErrorCase> cases = {
        {"ThinkSound audio generation", "this system: no supported GPU backend (Vulkan, ROCm, or CUDA) detected",
         "ThinkSound audio generation not supported by this system: no supported GPU backend (Vulkan, ROCm, or CUDA) detected"},
        {"TRELLIS 3D generation", "this system: no supported GPU backend (Vulkan, ROCm, or CUDA) detected",
         "TRELLIS 3D generation not supported by this system: no supported GPU backend (Vulkan, ROCm, or CUDA) detected"},
        {"ACE-Step audio generation", "this system: no supported GPU backend (Vulkan, ROCm, or CUDA) detected",
         "ACE-Step audio generation not supported by this system: no supported GPU backend (Vulkan, ROCm, or CUDA) detected"},
        {"OpenMOSS TTS", "this system: no supported GPU backend (Vulkan or CUDA) detected",
         "OpenMOSS TTS not supported by this system: no supported GPU backend (Vulkan or CUDA) detected"},
    };
    for (const auto& item : cases) {
        bool exact = false;
        try {
            first_supported_backend({}, item.operation, item.target);
        } catch (const UnsupportedOperationException& error) {
            exact = std::string(error.what()) == item.message && error.type() == "unsupported_operation" &&
                error.to_json() == json{{"error", {{"message", item.message}, {"type", "unsupported_operation"}}}};
        }
        check(exact, item.operation + " exact exception, message, type and JSON");
    }
    for (bool do_not_upgrade : {false, true}) {
        WrapperFixture explicit_backend;
        explicit_backend.options.set_option("thinksound_backend", "rocm-stable");
        explicit_backend.binary.external.clear();
        check(explicit_backend.prepare(do_not_upgrade) == "managed path/server" && explicit_backend.supported_queries == 0,
              "explicit-backend fixture bypasses fallback supported query for do_not_upgrade=" + std::to_string(do_not_upgrade));
        check(explicit_backend.binary.events == std::vector<std::string>{"find_external", "install", "managed_path"},
              "caller flag does not introduce install suppression");
    }
    WrapperFixture fallback;
    fallback.options.set_option("thinksound_backend", "");
    check(fallback.prepare(false) == "external path/server" && fallback.supported_queries == 1 && fallback.binary.backend == "cuda",
          "empty-option fixture queries supported backends once and selects first");
}

void check_loader_strings() {
    struct LoaderCase {
        std::string executable;
        std::string extras;
        std::optional<std::string> inherited;
        bool windows;
        std::pair<std::string, std::string> expected;
    };
    const std::vector<LoaderCase> cases = {
        {"/bin", "", std::nullopt, false, {"LD_LIBRARY_PATH", "/bin"}},
        {"/bin", "", "", false, {"LD_LIBRARY_PATH", "/bin:"}},
        {"/bin", "", "/old", false, {"LD_LIBRARY_PATH", "/bin:/old"}},
        {"/bin", "/a:/b", "/old:/older", false, {"LD_LIBRARY_PATH", "/a:/b:/bin:/old:/older"}},
        {"/bin space/", u8"/żółć:/日本/", "old:", false, {"LD_LIBRARY_PATH", u8"/żółć:/日本/:/bin space/:old:"}},
        {"C:\\bin", "", std::nullopt, true, {"PATH", "C:\\bin"}},
        {"C:\\bin", "", "", true, {"PATH", "C:\\bin;"}},
        {"C:\\bin", "", "C:\\old", true, {"PATH", "C:\\bin;C:\\old"}},
        {"C:\\bin", "C:\\a;D:\\b", "C:\\old;D:\\older", true, {"PATH", "C:\\a;D:\\b;C:\\bin;C:\\old;D:\\older"}},
        {"C:\\bin space\\", u8"C:\\żółć;D:\\日本;", "old;", true, {"PATH", u8"C:\\żółć;D:\\日本;;C:\\bin space\\;old;"}},
        {"", "", std::nullopt, false, {"LD_LIBRARY_PATH", ""}},
        {"", "", "", true, {"PATH", ";"}},
        {"/same", "/same", "/same", false, {"LD_LIBRARY_PATH", "/same:/same:/same"}},
    };
    for (size_t i = 0; i < cases.size(); ++i) {
        const auto& item = cases[i];
        check(make_loader_path_override(item.executable, item.extras, item.inherited, item.windows) == item.expected,
              "literal loader string case " + std::to_string(i));
    }
}

class ScopedEnvironment {
public:
    ScopedEnvironment(const std::string& name, const std::string& value) : name_(name) {
        if (const char* old = std::getenv(name.c_str())) old_ = old;
        set(value);
    }
    ~ScopedEnvironment() {
        if (old_) set(*old_);
        else {
#ifdef _WIN32
            _putenv_s(name_.c_str(), "");
#else
            unsetenv(name_.c_str());
#endif
        }
    }
private:
    void set(const std::string& value) {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value.c_str());
#else
        setenv(name_.c_str(), value.c_str(), 1);
#endif
    }
    std::string name_;
    std::optional<std::string> old_;
};

Environment wrapper_environment_fixture(const std::string& backend, bool openmoss) {
    Environment env;
    if (!openmoss && backend == "rocm") {
        env.push_back(make_loader_path_override("/fixture/bin", "/runtime/a:/runtime/b", "/inherited", false));
    } else if (backend == "cuda") {
        env.push_back(make_loader_path_override("/fixture/bin", "", "/inherited", false));
        BackendUtils::apply_cuda_env_vars(env, "fixture");
    }
    return env;
}

void check_environment_order() {
    ScopedEnvironment visibility("CUDA_VISIBLE_DEVICES", "7");
    ScopedEnvironment prime("__NV_PRIME_RENDER_OFFLOAD", "");
    for (bool openmoss : {false, true}) {
        check(wrapper_environment_fixture("rocm", openmoss) == (openmoss ? Environment{} : Environment{{"LD_LIBRARY_PATH", "/runtime/a:/runtime/b:/fixture/bin:/inherited"}}),
              "exact rocm fixture respects OpenMOSS exclusion");
        Environment cuda = {{"LD_LIBRARY_PATH", "/fixture/bin:/inherited"}};
#ifdef __linux__
        cuda.emplace_back("__NV_PRIME_RENDER_OFFLOAD", "1");
#endif
        check(wrapper_environment_fixture("cuda", openmoss) == cuda, "loader precedes real CUDA additions and visibility override is preserved");
        for (const auto backend : {"vulkan", "rocm-stable", "rocm-nightly"}) {
            check(wrapper_environment_fixture(backend, openmoss).empty(), std::string("exact caller condition excludes ") + backend);
        }
    }
}

void check_production_call_boundaries() {
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    for (const std::string stem : {"thinksound", "trellis", "acestep", "openmoss"}) {
        std::ifstream input(root / "src/cpp/server/backends" / stem / (stem + "_server.cpp"));
        if (!input) throw std::runtime_error("Cannot read production wrapper fixture");
        const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const auto guard = source.find("if (backend.empty()) {");
        const auto guard_end = source.find("\n    }", guard);
        const auto query = source.find("SystemInfo::get_supported_backends(\"" + stem + "\")", guard);
        const auto selection = source.find("first_supported_backend(", query);
        check(guard < query && query < selection && selection < guard_end && guard_end != std::string::npos,
              stem + " production supported query and selection stay inside empty-option branch");
        const auto validation = source.find("RuntimeConfig::validate_backend_choice(\"" + stem + "\", backend)", guard_end);
        const auto resolution = source.find("resolve_external_or_managed_binary(", validation);
        check(guard_end < validation && validation < resolution && resolution != std::string::npos &&
              source.find("bool /*do_not_upgrade*/") != std::string::npos,
              stem + " production validates before binary resolution and ignores upgrade flag");
        const auto loader = source.find("auto prepend_loader_path");
        const auto cuda = source.find("backend == \"cuda\"", loader);
        const auto prepend = source.find("prepend_loader_path(\"\")", cuda);
        const auto additions = source.find("BackendUtils::apply_cuda_env_vars(env_vars, \"" + stem + "-server\")", prepend);
        check(cuda < prepend && prepend < additions && additions != std::string::npos,
              stem + " production loader precedes CUDA additions with original log tag");
        check((source.find("backend == \"rocm\"", loader) != std::string::npos) == (stem != "openmoss") &&
              source.find("backend.rfind", loader) == std::string::npos,
              stem + " production keeps exact rocm condition outside string helper");
        if (stem == "trellis") {
            check(source.find("utils::validate_custom_args(trellis_args, reserved_flags)") < resolution,
                  "Trellis custom argument validation remains before binary lookup");
        }
    }
}

}

int main() {
    try {
        check_binary_resolution();
        check_selection();
        check_loader_strings();
        check_environment_order();
        check_production_call_boundaries();
    } catch (const std::exception& error) {
        check(false, std::string("unexpected exception: ") + error.what());
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

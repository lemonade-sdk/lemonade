// AP2 (R1, R16): build_argv. Run: bash test/cpp/adapter_spec/run.sh ap2
#include "adapter_spec.h"
#include "check.h"

using namespace lemon::backends::adapter::specs;
using V = std::vector<std::string>;

static int body() {
    const std::map<std::string, std::string> vals = {
        {"{port}", "8003"}, {"{model_path}", "/models/my model.gguf"}, {"{device}", "1"}, {"{spec_dir}", "/opt/a"}};

    // args array: each element filled, one argv entry each, spaces in values kept in one entry
    CHECK(build_argv(json{{"args", {"-m", "{model_path}", "--port", "{port}"}}}, vals) ==
          (V{"-m", "/models/my model.gguf", "--port", "8003"}));
    CHECK(build_argv(json{{"args", {"--dev={device}", "{spec_dir}/cfg.json"}}}, vals) == (V{"--dev=1", "/opt/a/cfg.json"}));
    CHECK(build_argv(json{{"args", json::array()}}, vals).empty());
    // no args and no template: empty argv
    CHECK(build_argv(json::object(), vals).empty());

    // args_template only at spec_version 1 (or absent)
    CHECK(build_argv(json{{"args_template", "-m {model_path} --port {port}"}}, vals) ==
          (V{"-m", "/models/my", "model.gguf", "--port", "8003"}));
    CHECK(build_argv(json{{"spec_version", 1}, {"args_template", "--port {port}"}}, vals) == (V{"--port", "8003"}));
    THROWS(build_argv(json{{"spec_version", 2}, {"args_template", "--port {port}"}}, vals), "args_template");

    // wrong shapes
    THROWS(build_argv(json{{"args", "--port {port}"}}, vals), "args");
    THROWS(build_argv(json{{"args", {"--port", 8003}}}, vals), "args");

    // unknown placeholder is named
    THROWS(build_argv(json{{"args", {"--evil", "{shell}"}}}, vals), "{shell}");
    THROWS(build_argv(json{{"args", {"--x={HOME}"}}}, vals), "{HOME}");
    // known placeholder that lemond did not supply is named
    THROWS(build_argv(json{{"args", {"--ctx", "{ctx}"}}}, vals), "{ctx}");
    THROWS(build_argv(json{{"args_template", "--uuids {gpu_uuids}"}}, vals), "{gpu_uuids}");

    // wrong types are SpecErrors that name the field, never a json type_error
    THROWS(build_argv(json{{"args_template", 7}}, vals), "args_template");
    THROWS(build_argv(json{{"spec_version", "1"}, {"args_template", "--port {port}"}}, vals), "spec_version");
    THROWS(build_argv(json{{"spec_version", 1.5}, {"args_template", "--port {port}"}}, vals), "spec_version");

    // braces that are not a placeholder token pass through (JSON in an argument)
    CHECK(build_argv(json{{"args", {"--cfg", "{}"}}}, vals) == (V{"--cfg", "{}"}));
    return report("ap2");
}

int main() {
    try {
        return body();
    } catch (const std::exception& e) {
        std::cerr << "ap2: uncaught exception (a check above did not run): " << e.what() << "\n";
        report("ap2");
        return 2;
    }
}

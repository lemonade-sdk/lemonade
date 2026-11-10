// AP3 (R3): build_env. Run: bash test/cpp/adapter_spec/run.sh ap3
#include "adapter_spec.h"
#include "check.h"

using namespace lemon::backends::adapter::specs;
using E = std::vector<std::pair<std::string, std::string>>;

static int body() {
    const std::map<std::string, std::string> vals = {{"{device}", "GPU-abc"}, {"{spec_dir}", "/opt/eng"}};
    const std::vector<std::string> dirs = {"/opt/eng", "/mnt/raid0/engines/llama.cpp/b1"};

    CHECK(build_env(json(), vals, dirs).empty());
    CHECK(build_env(json::object(), vals, dirs).empty());
    // allowed by prefix, filled, in key order (nlohmann objects iterate keys sorted)
    CHECK(build_env(json{{"CUDA_VISIBLE_DEVICES", "{device}"}, {"GGML_CUDA_NO_PINNED", "1"}}, vals, dirs) ==
          (E{{"CUDA_VISIBLE_DEVICES", "GPU-abc"}, {"GGML_CUDA_NO_PINNED", "1"}}));
    CHECK(build_env(json{{"NAUTIBURN_CLASS_ID", "live-x"}, {"TZ", "UTC"}}, vals, dirs).size() == 2);

    // denied names are named in the message
    THROWS(build_env(json{{"LD_PRELOAD", "/tmp/x.so"}}, vals, dirs), "LD_PRELOAD");
    THROWS(build_env(json{{"PATH", "/tmp"}}, vals, dirs), "PATH");
    THROWS(build_env(json{{"PYTHONPATH", "/tmp"}}, vals, dirs), "PYTHONPATH");
    THROWS(build_env(json{{"CUDA", "1"}}, vals, dirs), "CUDA");  // a prefix needs its underscore

    // LD_LIBRARY_PATH: only absolute entries inside the engine's own dirs
    CHECK(build_env(json{{"LD_LIBRARY_PATH", "{spec_dir}/lib:/mnt/raid0/engines/llama.cpp/b1/cuda12-libs"}}, vals, dirs) ==
          (E{{"LD_LIBRARY_PATH", "/opt/eng/lib:/mnt/raid0/engines/llama.cpp/b1/cuda12-libs"}}));
    CHECK(build_env(json{{"LD_LIBRARY_PATH", "/opt/eng"}}, vals, dirs).size() == 1);   // the dir itself
    // empty entries are ignored by the check, and the value is returned as given (not rebuilt)
    CHECK(build_env(json{{"LD_LIBRARY_PATH", "/opt/eng/lib::"}}, vals, dirs) == (E{{"LD_LIBRARY_PATH", "/opt/eng/lib::"}}));
    CHECK(build_env(json{{"LD_LIBRARY_PATH", "/opt/eng::/opt/eng/lib"}}, vals, dirs) ==
          (E{{"LD_LIBRARY_PATH", "/opt/eng::/opt/eng/lib"}}));
    THROWS(build_env(json{{"LD_LIBRARY_PATH", "/tmp/evil"}}, vals, dirs), "/tmp/evil");
    THROWS(build_env(json{{"LD_LIBRARY_PATH", "/opt/engine2/lib"}}, vals, dirs), "/opt/engine2/lib");  // prefix, not a dir
    THROWS(build_env(json{{"LD_LIBRARY_PATH", "/opt/eng/../../tmp"}}, vals, dirs), "..");
    THROWS(build_env(json{{"LD_LIBRARY_PATH", "lib"}}, vals, dirs), "lib");

    // wrong shapes
    THROWS(build_env(json::array({"CUDA_VISIBLE_DEVICES=0"}), vals, dirs), "env");
    THROWS(build_env(json{{"CUDA_VISIBLE_DEVICES", 0}}, vals, dirs), "CUDA_VISIBLE_DEVICES");
    return report("ap3");
}

int main() {
    try {
        return body();
    } catch (const std::exception& e) {
        std::cerr << "ap3: uncaught exception (a check above did not run): " << e.what() << "\n";
        report("ap3");
        return 2;
    }
}

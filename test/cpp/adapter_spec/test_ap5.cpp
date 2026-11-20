// AP5 (R16): check_spec. Run: bash test/cpp/adapter_spec/run.sh ap5
#include "adapter_spec.h"
#include "check.h"

using namespace lemon::backends::adapter::specs;

static bool has(const SpecCheck& c, const std::string& needle) {
    for (const auto& p : c.problems) if (p.find(needle) != std::string::npos) return true;
    return false;
}

static int body() {
    const json good = {{"spec_version", 1}, {"binary", "bin/llama-server"}, {"args", {"--port", "{port}"}}};
    auto c = check_spec(good);
    CHECK(c.level_cap == 4 && c.problems.empty());
    // args_template instead of args is fine; unknown fields are ignored
    c = check_spec(json{{"binary", "x"}, {"args_template", "--port {port}"}, {"x_vendor_note", 7}, {"future", {{"a", 1}}}});
    CHECK(c.level_cap == 4 && c.problems.empty());
    // all optional fields with the right types
    c = check_spec(json{{"binary", "x"}, {"args", json::array()}, {"env", json::object()}, {"ready", json::object()},
                        {"telemetry", json::object()}, {"telemetry_path", "/t"}, {"memory_path", "/m"},
                        {"budget_path", "/b"}, {"binary_sha256", std::string(64, 'a')}, {"default_device_index", "1"}});
    CHECK(c.level_cap == 4 && c.problems.empty());

    // missing required
    c = check_spec(json{{"args", json::array()}});
    CHECK(c.level_cap == 0 && has(c, "binary"));
    c = check_spec(json{{"binary", ""}, {"args", json::array()}});
    CHECK(c.level_cap == 0 && has(c, "binary"));
    c = check_spec(json{{"binary", "x"}});
    CHECK(c.level_cap == 0 && has(c, "args"));
    // newer schema
    c = check_spec(json{{"spec_version", kSpecVersion + 1}, {"binary", "x"}, {"args", json::array()}});
    CHECK(c.level_cap == 0 && has(c, "spec_version"));
    c = check_spec(json{{"spec_version", 0}, {"binary", "x"}, {"args", json::array()}});
    CHECK(c.level_cap == 0 && has(c, "spec_version"));
    c = check_spec(json{{"spec_version", "1"}, {"binary", "x"}, {"args", json::array()}});
    CHECK(c.level_cap == 0 && has(c, "spec_version"));
    // wrong types, one problem each
    c = check_spec(json{{"binary", "x"}, {"args", "--port"}, {"env", json::array()}, {"binary_sha256", "abc"}});
    CHECK(c.level_cap == 0 && c.problems.size() == 3 && has(c, "args") && has(c, "env") && has(c, "binary_sha256"));
    c = check_spec(json{{"binary", "x"}, {"args", json::array()}, {"binary_sha256", std::string(64, 'g')}});
    CHECK(c.level_cap == 0 && has(c, "binary_sha256"));
    c = check_spec(json{{"binary", "x"}, {"args", json::array()}, {"default_device_index", 1}});
    CHECK(c.level_cap == 0 && has(c, "default_device_index"));
    // not an object
    c = check_spec(json::array());
    CHECK(c.level_cap == 0 && c.problems.size() == 1);
    return report("ap5");
}

int main() {
    try {
        return body();
    } catch (const std::exception& e) {
        std::cerr << "ap5: uncaught exception (a check above did not run): " << e.what() << "\n";
        report("ap5");
        return 2;
    }
}

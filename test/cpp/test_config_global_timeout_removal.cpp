// Regression: removing global_timeout must not break existing installations.
#include <cstdio>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <lemon/runtime_config.h>

int main() {
    using json = nlohmann::json;
    using lemon::RuntimeConfig;
    int failed = 0;
    const auto check = [&failed](bool condition, const char* description) {
        std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", description);
        if (!condition) ++failed;
    };

    RuntimeConfig cfg(json{{"global_timeout", 3600}, {"port", 13305}});
    check(!cfg.snapshot().contains("global_timeout"),
          "obsolete persisted key is discarded on load");
    check(cfg.snapshot().value("port", 0) == 13305,
          "unrelated persisted configuration survives");

    bool rejected = false;
    try {
        cfg.set(json{{"global_timeout", 3600}});
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "new global_timeout writes are rejected");
    check(!cfg.snapshot().contains("global_timeout"),
          "rejected write does not reinstate obsolete key");
    return failed == 0 ? 0 : 1;
}

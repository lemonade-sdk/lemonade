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
    check(cfg.inference_inactivity_timeout_seconds() == 1800,
          "default inference idle timeout remains active");
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
    check(cfg.backend_startup_timeout_seconds() == 0,
          "zero selects the backend-specific startup timeout");
    cfg.set(json{{"backend_startup_timeout_seconds", 3600}});
    check(cfg.backend_startup_timeout_seconds() == 3600,
          "startup timeout can be set through config");
    cfg.set(json{{"backend_startup_timeout_seconds", 0}});
    check(cfg.backend_startup_timeout_seconds() == 0,
          "zero restores backend-specific startup timeout");

    for (const json& bad : {json(-1), json(86401), json("3600"), json(1.5)}) {
        bool invalid_rejected = false;
        try {
            cfg.set(json{{"backend_startup_timeout_seconds", bad}});
        } catch (const std::invalid_argument&) {
            invalid_rejected = true;
        }
        check(invalid_rejected, "out-of-range or non-integer startup timeout rejected");
    }
    check(cfg.backend_startup_timeout_seconds() == 0,
          "invalid config writes preserve the previous timeout");
    bool invalid_persisted_rejected = false;
    try {
        RuntimeConfig invalid(json{{"backend_startup_timeout_seconds", -1}});
    } catch (const std::invalid_argument&) {
        invalid_persisted_rejected = true;
    }
    check(invalid_persisted_rejected, "invalid persisted startup timeout rejected");

    cfg.set(json{{"inference_inactivity_timeout_seconds", 3600}});
    check(cfg.inference_inactivity_timeout_seconds() == 3600,
          "inference idle limit can be extended");
    cfg.set(json{{"inference_inactivity_timeout_seconds", 0}});
    check(cfg.inference_inactivity_timeout_seconds() == 0,
          "inference idle guard can be disabled explicitly");
    for (const json& bad : {json(-1), json(86401), json("3600"), json(1.5)}) {
        bool invalid_rejected = false;
        try {
            cfg.set(json{{"inference_inactivity_timeout_seconds", bad}});
        } catch (const std::invalid_argument&) {
            invalid_rejected = true;
        }
        check(invalid_rejected, "invalid inference idle timeout rejected");
    }
    check(cfg.inference_inactivity_timeout_seconds() == 0,
          "rejected inference config writes preserve the previous value");
    bool invalid_idle_persisted_rejected = false;
    try {
        RuntimeConfig invalid(json{{"inference_inactivity_timeout_seconds", -1}});
    } catch (const std::invalid_argument&) {
        invalid_idle_persisted_rejected = true;
    }
    check(invalid_idle_persisted_rejected, "invalid persisted inference idle timeout rejected");
    return failed == 0 ? 0 : 1;
}

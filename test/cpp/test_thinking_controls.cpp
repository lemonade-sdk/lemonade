// Proves how a request that disables thinking is translated before dispatch:
// the generic controls with the /no_think prompt fallback, or the fields a
// backend names as its own.
// Build with: cmake --build --preset default --target test_thinking_controls
// Run with: ctest --test-dir build -R '^ThinkingControlsTest$' --output-on-failure

#include "lemon/thinking_controls.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(const std::string& what, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

lemon::json disabled_request() {
    return {{"thinking", {{"type", "disabled"}}},
            {"messages", {{{"role", "user"}, {"content", "hello"}}}}};
}

}  // namespace

int main() {
    lemon::json with_fallback = disabled_request();
    lemon::normalize_thinking_controls(with_fallback);
    check("native controls are set", with_fallback.value("reasoning_effort", "") == "none" &&
                                         with_fallback["chat_template_kwargs"]["enable_thinking"] ==
                                             false);
    check("the prompt fallback prefixes the user message",
          with_fallback["messages"][0]["content"] == "/no_think\nhello");
    check("the client-facing field is stripped", !with_fallback.contains("thinking"));

    lemon::json backend_own = disabled_request();
    lemon::normalize_thinking_controls(
        backend_own, {{"chat_template_kwargs", {{"enable_thinking", false}}}});
    check("the backend's own fields are set",
          backend_own["chat_template_kwargs"]["enable_thinking"] == false);
    check("the generic controls the backend did not name are left out",
          !backend_own.contains("reasoning_effort"));
    check("the user message is left as the client sent it",
          backend_own["messages"][0]["content"] == "hello");
    check("the client-facing field is stripped with backend fields too",
          !backend_own.contains("thinking"));

    if (failures == 0) {
        std::printf("\nAll thinking control checks passed.\n");
        return 0;
    }
    std::printf("\n%d thinking control check(s) failed.\n", failures);
    return 1;
}

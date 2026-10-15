#include "configured_fixture.h"
#include "lemon/backends/configured/configured_server.h"
#include "lemon/backends/ds4/ds4_server.h"
#include "lemon/backends/thinksound/thinksound_server.h"

#include <cstdlib>

using namespace thinksound_oracle;
using namespace lemon::backends;

namespace {
bool enabled(const char* key) { const auto* value = std::getenv(key); return value && std::string(value) == "1"; }
void invert(const char* key, bool value) {
#ifdef _WIN32
    _putenv_s(key, value ? "0" : "1");
#else
    setenv(key, value ? "0" : "1", 1);
#endif
}
}
int main() {
    const bool thinksound_enabled = enabled("LEMONADE_THINKSOUND_CONFIGURED");
    const bool ds4_enabled = enabled("LEMONADE_DS4_CONFIGURED");
    for (int pass = 0; pass < 3; ++pass) {
        auto thinksound = create_server("thinksound", BackendContext{"error"});
        auto ds4 = create_server("ds4", BackendContext{"error"});
        check((dynamic_cast<ConfiguredServer*>(thinksound.get()) != nullptr) == thinksound_enabled && (dynamic_cast<ThinkSoundServer*>(thinksound.get()) != nullptr) == !thinksound_enabled, "ThinkSound frozen factory arm");
        check((dynamic_cast<ConfiguredServer*>(ds4.get()) != nullptr) == ds4_enabled && (dynamic_cast<Ds4Server*>(ds4.get()) != nullptr) == !ds4_enabled, "DS4 frozen factory arm");
        check(bool(backends::thinksound::spec()->definition) == thinksound_enabled && (backends::thinksound::ops() != backends::thinksound::legacy_ops()) == thinksound_enabled, "ThinkSound spec/ops select same arm");
        check(bool(backends::ds4::spec()->definition) == ds4_enabled && (backends::ds4::ops() != backends::ds4::legacy_ops()) == ds4_enabled, "DS4 spec/ops select same arm");
        check(backends::thinksound::spec()->recipe == "thinksound" && backends::thinksound::spec()->binary == "ts-server" && backends::ds4::spec()->recipe == "ds4" && backends::ds4::spec()->binary == "ds4-server", "alternating registration independent identities");
        invert("LEMONADE_THINKSOUND_CONFIGURED", thinksound_enabled); invert("LEMONADE_DS4_CONFIGURED", ds4_enabled);
    }
    std::printf("%d/%d checks passed\n", checks - failures, checks); return failures ? 1 : 0;
}

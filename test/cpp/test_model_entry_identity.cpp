#include <lemon/model_manager.h>
#include <lemon/models/entry_identity.h>

#include <cstdio>
#include <string>

using lemon::ModelInfo;
using lemon::ModelSource;
using lemon::models::entry_identity;
using lemon::models::entry_precedence_rank;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static ModelInfo cloud(const std::string& key, const std::string& provider) {
    ModelInfo info;
    info.model_name = key;
    info.recipe = "cloud";
    info.cloud_provider = provider;
    return info;
}

int main() {
    {
        ModelInfo info;
        const auto registered = entry_identity("user.Coder", info);
        check("user. parses to Registered", registered.source == ModelSource::Registered);
        check("user. bare drops the prefix", registered.bare == "Coder");
        check("user. is not self-qualified", !registered.qualifies_itself);

        const auto imported = entry_identity("extra.Scan-Me", info);
        check("extra. parses to Imported", imported.source == ModelSource::Imported);
        check("extra. bare drops the prefix", imported.bare == "Scan-Me");

        const auto builtin = entry_identity("Plain-Model", info);
        check("a bare key is Builtin", builtin.source == ModelSource::Builtin);
        check("a bare key keeps its name", builtin.bare == "Plain-Model");
        check("a bare key is not self-qualified", !builtin.qualifies_itself);
    }

    {
        // A provider listing already qualified the id, and `fireworks.` is none of
        // the handle prefixes: parsing it used to yield Builtin with the whole key
        // as the bare name, which then earned a `builtin.fireworks.X` alias.
        const auto c = entry_identity("fireworks.kimi-k2p5",
                                      cloud("fireworks.kimi-k2p5", "fireworks"));
        check("a cloud entry qualifies itself", c.qualifies_itself);
        check("a cloud entry keeps its qualified name", c.bare == "fireworks.kimi-k2p5");
    }

    {
        ModelInfo plain;
        const auto registered = entry_identity("user.X", plain);
        const auto c = entry_identity("fireworks.X", cloud("fireworks.X", "fireworks"));
        const auto imported = entry_identity("extra.X", plain);
        const auto builtin = entry_identity("X", plain);

        check("a registration outranks a cloud model",
              entry_precedence_rank(registered) < entry_precedence_rank(c));
        check("a cloud model outranks a scanned file",
              entry_precedence_rank(c) < entry_precedence_rank(imported));
        check("a scanned file outranks a built-in",
              entry_precedence_rank(imported) < entry_precedence_rank(builtin));
    }

    if (g_failures == 0) {
        std::printf("All entry identity tests passed.\n");
    } else {
        std::printf("%d entry identity test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}

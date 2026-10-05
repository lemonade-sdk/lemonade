// Standalone test for the shared image-gen request helpers: the typed
// RecipeOptions getters and lemon/image_params.h. These pin the guarded-read
// behavior (mistyped stored options fall back instead of throwing) and the
// explicit-zero semantics that build_request()/build_extra_args() rely on.

#include <lemon/image_params.h>
#include <lemon/recipe_options.h>

#include <cstdio>
#include <string>

using json = nlohmann::json;
using lemon::RecipeOptions;
namespace ip = lemon::image_params;

static bool check(const char* name, bool condition,
                  const std::string& got = "", const std::string& want = "") {
    bool ok = condition;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok && !got.empty() && !want.empty()) {
        std::printf("  got:  %s\n  want: %s\n", got.c_str(), want.c_str());
    }
    return ok;
}

static int fail(const char* name, bool condition,
                const std::string& got = "", const std::string& want = "") {
    return check(name, condition, got, want) ? 0 : 1;
}

int main() {
    int failures = 0;

    // --- RecipeOptions typed getters ---

    // Empty options resolve through to the descriptor defaults.
    {
        RecipeOptions opts("sd-cpp", json::object());
        failures += fail("get_int_or: unset resolves to descriptor default",
            opts.get_int_or("steps") == 20, std::to_string(opts.get_int_or("steps")), "20");
        failures += fail("get_float_or: unset resolves to descriptor default",
            opts.get_float_or("cfg_scale") == 7.0f,
            std::to_string(opts.get_float_or("cfg_scale")), "7.0");
        failures += fail("get_string_or: unset resolves to descriptor default (empty)",
            opts.get_string_or("sampler").empty(), opts.get_string_or("sampler"), "");
        failures += fail("get_int_or: unknown key returns fallback",
            opts.get_int_or("no_such_option", 42) == 42);
    }

    // Mistyped stored values (config is never type-checked) fall back instead
    // of throwing nlohmann type_error.
    {
        RecipeOptions opts("sd-cpp", json::object());
        opts.set_option("steps", "30");
        opts.set_option("cfg_scale", "high");
        opts.set_option("sampler", 7);
        opts.set_option("flow_shift", "lots");
        failures += fail("get_int_or: string storage returns fallback, no throw",
            opts.get_int_or("steps", 0) == 0);
        failures += fail("get_float_or: string storage returns fallback, no throw",
            opts.get_float_or("cfg_scale", 0.0f) == 0.0f);
        failures += fail("get_string_or: numeric storage returns fallback, no throw",
            opts.get_string_or("sampler").empty());
        failures += fail("has_numeric: string storage is not numeric",
            !opts.has_numeric("flow_shift"));
    }

    // Explicit zero is a real stored value (not a sentinel) and must survive.
    {
        RecipeOptions opts("sd-cpp", {{"cfg_scale", 0.0}, {"steps", 0}});
        failures += fail("has_numeric: explicitly stored 0.0 is numeric",
            opts.has_numeric("cfg_scale"));
        failures += fail("get_float_or: explicit 0.0 preserved",
            opts.get_float_or("cfg_scale", 9.0f) == 0.0f);
        failures += fail("get_int_or: explicit 0 preserved",
            opts.get_int_or("steps", 9) == 0);
    }

    // -1 is a dropped sentinel: resolution skips it and lands on the
    // descriptor default.
    {
        RecipeOptions opts("sd-cpp", {{"steps", -1}});
        failures += fail("get_int_or: sentinel -1 falls through to descriptor default",
            opts.get_int_or("steps") == 20, std::to_string(opts.get_int_or("steps")), "20");
    }

    // has_numeric on a key whose descriptor default is not a number.
    {
        RecipeOptions opts("sd-cpp", json::object());
        failures += fail("has_numeric: descriptor default 7.0 is numeric",
            opts.has_numeric("cfg_scale"));
        failures += fail("has_numeric: empty-string default is not numeric",
            !opts.has_numeric("sampling_method"));
    }

    // get_bool_or guards against non-bool storage (the qwen_vae_enhance crash).
    {
        RecipeOptions opts("thenoise", {{"qwen_vae_enhance", true}});
        failures += fail("get_bool_or: stored true preserved",
            opts.get_bool_or("qwen_vae_enhance"));
        RecipeOptions bad("thenoise", json::object());
        bad.set_option("qwen_vae_enhance", "true");
        failures += fail("get_bool_or: string storage returns fallback, no throw",
            !bad.get_bool_or("qwen_vae_enhance", false));
    }

    // --- image_params::resolve_size ---

    {
        RecipeOptions empty("sd-cpp", json::object());

        json req_size = {{"size", "768x512"}};
        failures += fail("resolve_size: request size string wins",
            ip::resolve_size(req_size, empty) == "768x512");

        json req_wh = {{"width", 640}, {"height", 480}};
        failures += fail("resolve_size: request width/height pair",
            ip::resolve_size(req_wh, empty) == "640x480");

        json none = json::object();
        failures += fail("resolve_size: falls back to descriptor default 512x512",
            ip::resolve_size(none, empty) == "512x512");

        RecipeOptions sized("sd-cpp", {{"width", 1024}, {"height", 768}});
        failures += fail("resolve_size: recipe options beat descriptor default",
            ip::resolve_size(none, sized) == "1024x768");

        RecipeOptions typed("sd-cpp", json::object());
        typed.set_option("width", "wide");
        failures += fail("resolve_size: mistyped width yields empty, no throw",
            ip::resolve_size(none, typed).empty());

        json half = {{"width", 640}};
        failures += fail("resolve_size: single request dimension ignored (pre-existing)",
            ip::resolve_size(half, empty) == "512x512");
    }

    // --- image_params::resolve_seed ---

    {
        json none = json::object();
        failures += fail("resolve_seed: absent yields nullopt",
            !ip::resolve_seed(none).has_value());

        json pos = {{"seed", 12345}};
        failures += fail("resolve_seed: positive seed passes through",
            ip::resolve_seed(pos) == 12345);

        json zero = {{"seed", 0}};
        failures += fail("resolve_seed: zero seed passes through",
            ip::resolve_seed(zero) == 0);

        json neg = {{"seed", -1}};
        auto seed = ip::resolve_seed(neg);
        failures += fail("resolve_seed: negative seed replaced by concrete value",
            seed.has_value() && *seed >= 0 && *seed <= 0x7fffffff);

        json flt = {{"seed", 1.5}};
        failures += fail("resolve_seed: non-integer yields nullopt",
            !ip::resolve_seed(flt).has_value());
    }

    // --- image_params request-body readers ---

    {
        json req = {
            {"steps", 8},
            {"cfg_scale", 0.0},
            {"sampler", "euler"},
            {"qwen", true},
            {"steps_str", "8"},
            {"steps_float", 8.5},
            {"cfg_str", "high"},
        };
        json none = json::object();

        failures += fail("request_int: integer present",
            ip::request_int(req, "steps", 1) == 8);
        failures += fail("request_int: float is not an integer",
            ip::request_int(req, "steps_float", 1) == 1);
        failures += fail("request_int: string is not an integer",
            ip::request_int(req, "steps_str", 1) == 1);
        failures += fail("request_int: absent yields fallback",
            ip::request_int(none, "steps", 1) == 1);

        auto cfg = ip::request_number(req, "cfg_scale");
        failures += fail("request_number: explicit 0.0 present as value",
            cfg.has_value() && *cfg == 0.0f);
        failures += fail("request_number: string yields nullopt",
            !ip::request_number(req, "cfg_str").has_value());
        failures += fail("request_number: absent yields nullopt",
            !ip::request_number(none, "cfg_scale").has_value());

        failures += fail("request_string: string present",
            ip::request_string(req, "sampler", "x") == "euler");
        failures += fail("request_string: numeric value yields fallback",
            ip::request_string(req, "steps", "x") == "x");

        failures += fail("request_bool: bool present",
            ip::request_bool(req, "qwen", false));
        failures += fail("request_bool: numeric value yields fallback",
            !ip::request_bool(req, "steps", false));
    }

    std::printf("\n%s\n", failures == 0 ? "ALL TESTS PASSED" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}

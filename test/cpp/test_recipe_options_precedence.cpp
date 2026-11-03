// Standalone test for RecipeOptions precedence: merge_precedence_layers() folds
// one model's layers, inherit() folds the router's. Custom-argument scope
// merging is separate, in resolve_scoped_custom_args().

#include <lemon/recipe_options.h>

#include <cstdio>
#include <string>

using json = nlohmann::json;
using lemon::RecipeOptions;

static bool check(const char* name, bool condition,
                  const std::string& got = "", const std::string& want = "") {
    bool ok = condition;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
        if (!got.empty() && !want.empty()) {
            std::printf("  got:  %s\n  want: %s\n", got.c_str(), want.c_str());
        }
    }
    return ok;
}

static int fail(const char* name, bool condition,
                const std::string& got, const std::string& want) {
    return check(name, condition, got, want) ? 0 : 1;
}

int main() {
    int failures = 0;

    static const json model_layer = {
        {"steps", 4},
        {"cfg_scale", 1.0f},
        {"width", 512},
        {"height", 512},
        {"sampling_method", "euler"},
        {"flow_shift", 3.0f}
    };

    // --- merge_precedence_layers: user-saved > what the row authored ---

    // A1: the lowest layer survives when the layer above it is null or empty
    {
        RecipeOptions merged = RecipeOptions::merge_precedence_layers(
            "sd-cpp", {model_layer, json::object()});

        failures += fail("layers: steps preserved",
            merged.get_option("steps").get<int>() == 4,
            std::to_string(merged.get_option("steps").get<int>()), "4");
        failures += fail("layers: cfg_scale preserved",
            merged.get_option("cfg_scale").get<float>() == 1.0f,
            std::to_string(merged.get_option("cfg_scale").get<float>()), "1.0");
        failures += fail("layers: width preserved",
            merged.get_option("width").get<int>() == 512,
            std::to_string(merged.get_option("width").get<int>()), "512");
        failures += fail("layers: sampling_method preserved",
            merged.get_option("sampling_method").get<std::string>() == "euler",
            merged.get_option("sampling_method").get<std::string>(), "euler");
        failures += fail("layers: flow_shift preserved",
            merged.get_option("flow_shift").get<float>() == 3.0f,
            std::to_string(merged.get_option("flow_shift").get<float>()), "3.0");
    }

    // A2: the layer above overrides the keys it names, and only those
    {
        json model_ro = {
            {"steps", 12},
            {"cfg_scale", 3.5f},
            {"sdcpp_args", "--diffusion-fa"},
        };

        RecipeOptions merged = RecipeOptions::merge_precedence_layers(
            "sd-cpp", {model_layer, model_ro, json(nullptr)});

        failures += fail("layers: upper-layer steps wins",
            merged.get_option("steps").get<int>() == 12,
            std::to_string(merged.get_option("steps").get<int>()), "12");
        failures += fail("layers: upper-layer cfg_scale wins",
            merged.get_option("cfg_scale").get<float>() == 3.5f,
            std::to_string(merged.get_option("cfg_scale").get<float>()), "3.5");
        failures += fail("layers: sdcpp_args present",
            merged.get_option("sdcpp_args").get<std::string>() == "--diffusion-fa",
            merged.get_option("sdcpp_args").get<std::string>(), "--diffusion-fa");
        failures += fail("layers: width kept when not overridden",
            merged.get_option("width").get<int>() == 512,
            std::to_string(merged.get_option("width").get<int>()), "512");
    }

    // A3: user-saved options override every model-level layer
    {
        json model_ro = {{"steps", 12}};
        json user_saved = {
            {"steps", 20},
            {"width", 1024},
            {"sdcpp_args", "--mmap"},
        };

        RecipeOptions merged = RecipeOptions::merge_precedence_layers(
            "sd-cpp", {model_layer, model_ro, user_saved});

        failures += fail("layers: user-saved steps beats the model layer",
            merged.get_option("steps").get<int>() == 20,
            std::to_string(merged.get_option("steps").get<int>()), "20");
        failures += fail("layers: user-saved width beats the model layer",
            merged.get_option("width").get<int>() == 1024,
            std::to_string(merged.get_option("width").get<int>()), "1024");
        failures += fail("layers: cfg_scale kept when not overridden",
            merged.get_option("cfg_scale").get<float>() == 1.0f,
            std::to_string(merged.get_option("cfg_scale").get<float>()), "1.0");
    }

    // A4: *_args keys are replaced wholesale at the model level (no token merge).
    // Token merging happens later, in inherit(), between router layers.
    {
        json model_ro = {{"sdcpp_args", "--threads 8 --mmap"}};
        json user_saved = {{"sdcpp_args", "--threads 4"}};

        RecipeOptions merged = RecipeOptions::merge_precedence_layers(
            "sd-cpp", {model_layer, model_ro, user_saved});

        std::string args = merged.get_option("sdcpp_args").get<std::string>();
        failures += fail("args: user-saved sdcpp_args fully replaces model-level",
            args == "--threads 4", args, "--threads 4");
    }

    // A5: no layers at all — empty result, get_option falls back to defaults
    {
        json null_opts = json(nullptr);
        RecipeOptions merged = RecipeOptions::merge_precedence_layers(
            "sd-cpp", {null_opts, null_opts, null_opts});

        failures += fail("layers: all-null layers yield no options",
            merged.to_json().is_object() && merged.to_json().empty(),
            merged.to_json().dump(), "{}");
    }

    // --- inherit: per-request > model > arch > global (the router's fold) ---

    // B1: the calling (higher) layer wins; the parent fills in gaps
    {
        json parent_opts = {{"steps", 20}, {"cfg_scale", 7.0f}};
        json child_opts = {{"steps", 10}};

        RecipeOptions parent("sd-cpp", parent_opts);
        RecipeOptions child("sd-cpp", child_opts);
        RecipeOptions merged = child.inherit(parent);

        failures += fail("inherit: higher layer steps wins",
            merged.get_option("steps").get<int>() == 10,
            std::to_string(merged.get_option("steps").get<int>()), "10");
        failures += fail("inherit: parent cfg_scale inherited",
            merged.get_option("cfg_scale").get<float>() == 7.0f,
            std::to_string(merged.get_option("cfg_scale").get<float>()), "7.0");
    }

    // B2: *_args follow normal inherit precedence here. Scope-aware custom-arg
    // merging is handled separately by resolve_scoped_custom_args().
    {
        json higher = {{"sdcpp_args", "--threads 8 --cache-type q8_0"}};
        json lower = {{"sdcpp_args", "--threads 4 --mmap"}};
        RecipeOptions a("sd-cpp", higher);
        RecipeOptions b("sd-cpp", lower);
        RecipeOptions merged = a.inherit(b);
        std::string args = merged.get_option("sdcpp_args").get<std::string>();
        failures += fail(
            "inherit: higher-layer sdcpp_args wins wholesale",
            args == "--threads 8 --cache-type q8_0",
            args, "--threads 8 --cache-type q8_0");
    }

    // B3: an empty higher layer does not override the lower layer
    {
        RecipeOptions lower("sd-cpp", model_layer);
        RecipeOptions empty("sd-cpp", json::object());
        RecipeOptions merged = empty.inherit(lower);

        failures += fail("inherit: empty layer leaves parent steps intact",
            merged.get_option("steps").get<int>() == 4,
            std::to_string(merged.get_option("steps").get<int>()), "4");
        failures += fail("inherit: empty layer leaves parent cfg_scale intact",
            merged.get_option("cfg_scale").get<float>() == 1.0f,
            std::to_string(merged.get_option("cfg_scale").get<float>()), "1.0");
    }

    // B4: empty values (-1, "") are filtered out and inherit from the lower
    // layer; 0.0 is a real value and stays
    {
        json parent_opts = {{"steps", 4}, {"cfg_scale", 1.0f}, {"sampling_method", "euler"}};
        json child_opts = {{"steps", -1}, {"cfg_scale", 0.0f}, {"sampling_method", ""}};

        RecipeOptions parent("sd-cpp", parent_opts);
        RecipeOptions child("sd-cpp", child_opts);
        RecipeOptions merged = child.inherit(parent);

        failures += fail("inherit: -1 steps treated as unset, parent value inherited",
            merged.get_option("steps").get<int>() == 4,
            std::to_string(merged.get_option("steps").get<int>()), "4");
        failures += fail("inherit: empty-string sampling_method treated as unset",
            merged.get_option("sampling_method").get<std::string>() == "euler",
            merged.get_option("sampling_method").get<std::string>(), "euler");
        failures += fail("inherit: 0.0 cfg_scale is a real value and is kept",
            merged.get_option("cfg_scale").get<float>() == 0.0f,
            std::to_string(merged.get_option("cfg_scale").get<float>()), "0.0");
    }

    // --- has_option: "set in a layer" vs. descriptor-default fallback ---
    // Backends use has_option() to decide whether to send a value at all;
    // unset values fall through to the backend's own native defaults.

    // C1: a layer sets only steps; cfg_scale stays unset even though get_option
    // would resolve it to the descriptor default (7.0)
    {
        json only_steps = {{"steps", 8}};
        RecipeOptions merged = RecipeOptions::merge_precedence_layers(
            "sd-cpp", {only_steps, json(nullptr), json(nullptr)});

        failures += fail("has_option: steps set by a layer",
            merged.has_option("steps"), "", "");
        failures += fail("has_option: cfg_scale not set by any layer",
            !merged.has_option("cfg_scale"), "", "");
        failures += fail("has_option: unset option still resolves to descriptor default",
            merged.get_option("cfg_scale").get<float>() == 7.0f,
            std::to_string(merged.get_option("cfg_scale").get<float>()), "7.0");
    }

    // C2: no layers at all — nothing is set, everything falls through
    {
        RecipeOptions merged = RecipeOptions::merge_precedence_layers(
            "sd-cpp", {json(nullptr), json(nullptr), json(nullptr)});

        failures += fail("has_option: no layers means steps unset",
            !merged.has_option("steps"), "", "");
        failures += fail("has_option: no layers means width unset",
            !merged.has_option("width"), "", "");
    }

    // C3: inherit propagates set-ness from the lower layer
    {
        json parent_opts = {{"steps", 20}};
        RecipeOptions parent("sd-cpp", parent_opts);
        RecipeOptions child("sd-cpp", json::object());
        RecipeOptions merged = child.inherit(parent);

        failures += fail("has_option: key inherited from lower layer is set",
            merged.has_option("steps"), "", "");
        failures += fail("has_option: key absent everywhere stays unset",
            !merged.has_option("cfg_scale"), "", "");
    }

    // --- typed reads: a stored value of the wrong type reads as unset ---
    // No writer validates an option value's type except the options route, and
    // get_option() hands whatever it holds to a cast that throws. These reads
    // drop a value they cannot use, so the load falls back the same way it does
    // for a key no layer named. has_option() is deliberately untouched: a
    // backend that gates on it still sees the key as named.

    // D1: values no cast would survive
    {
        json malformed = {
            {"steps", "eight"},
            {"cfg_scale", true},
            {"sampling_method", 7},
            {"width", 512.0},
        };
        RecipeOptions opts("sd-cpp", malformed);

        failures += fail("typed: non-numeric steps falls back to the descriptor default",
            opts.int_option("steps").value_or(-1) == 20,
            std::to_string(opts.int_option("steps").value_or(-1)), "20");
        failures += fail("typed: non-numeric cfg_scale falls back to the descriptor default",
            opts.number_option("cfg_scale").value_or(-1.0) == 7.0,
            std::to_string(opts.number_option("cfg_scale").value_or(-1.0)), "7.0");
        failures += fail("typed: non-string sampling_method falls back to the descriptor default",
            opts.string_option("sampling_method").value_or("?") == "",
            opts.string_option("sampling_method").value_or("?"), "(empty)");
        failures += fail("typed: fractional width truncates rather than falling back",
            opts.int_option("width").value_or(0) == 512,
            std::to_string(opts.int_option("width").value_or(0)), "512");
    }

    // D2: values a cast already handled are returned as they were stored
    {
        RecipeOptions opts("sd-cpp", model_layer);

        failures += fail("typed: steps read as int", opts.int_option("steps").value_or(-1) == 4,
                         std::to_string(opts.int_option("steps").value_or(-1)), "4");
        failures += fail("typed: cfg_scale read as double", opts.number_option("cfg_scale").value_or(-1.0) == 1.0,
                         std::to_string(opts.number_option("cfg_scale").value_or(-1.0)), "1.0");
        failures += fail("typed: sampling_method read as string",
                         opts.string_option("sampling_method").value_or("?") == "euler",
                         opts.string_option("sampling_method").value_or("?"), "euler");
    }

    // D3: unset resolves through to the descriptor default; an option no
    // descriptor declares has nothing to fall back to
    {
        RecipeOptions opts("sd-cpp", json::object());

        failures += fail("typed: unset steps resolves to the descriptor default",
                         opts.int_option("steps").value_or(-1) == 20,
                         std::to_string(opts.int_option("steps").value_or(-1)), "20");
        failures += fail("typed: unset option with no default is nullopt",
                         !opts.int_option("not_an_option").has_value(), "", "");
        failures += fail("typed: unset option with no default reads as no string either",
                         !opts.string_option("not_an_option").has_value(), "", "");
    }

    // D4: a bool option stored as something else
    {
        json wrong_type = {{"qwen_vae_enhance", "yes"}};
        RecipeOptions opts("thenoise", wrong_type);

        failures += fail("typed: non-bool qwen_vae_enhance falls back to the descriptor default",
                         opts.bool_option("qwen_vae_enhance").value_or(true) == false, "", "");

        json set_true = {{"qwen_vae_enhance", true}};
        RecipeOptions enabled("thenoise", set_true);
        failures += fail("typed: bool read back as stored",
                         enabled.bool_option("qwen_vae_enhance").value_or(false), "", "");
    }

    std::printf("\n%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

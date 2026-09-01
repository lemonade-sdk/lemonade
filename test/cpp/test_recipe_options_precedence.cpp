// Standalone test for RecipeOptions precedence. The full ladder is documented
// in SDServer::build_extra_args(); these cases pin merge_precedence_layers()
// Custom-argument scope merging is handled separately by
// resolve_scoped_custom_args().

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

    // A model entry's image_defaults, as extracted from server_models.json
    static const json image_defaults = {
        {"steps", 4},
        {"cfg_scale", 1.0f},
        {"width", 512},
        {"height", 512},
        {"sampling_method", "euler"},
        {"flow_shift", 3.0f}
    };

    // --- merge_precedence_layers: user-saved > model recipe_options > image_defaults ---

    // A1: image_defaults survive when the upper layers are null or empty
    {
        json null_opts = json(nullptr);
        RecipeOptions merged = RecipeOptions::merge_precedence_layers(
            "sd-cpp", image_defaults, null_opts, json::object());

        failures += fail("layers: image_defaults steps preserved",
            merged.get_option("steps").get<int>() == 4,
            std::to_string(merged.get_option("steps").get<int>()), "4");
        failures += fail("layers: image_defaults cfg_scale preserved",
            merged.get_option("cfg_scale").get<float>() == 1.0f,
            std::to_string(merged.get_option("cfg_scale").get<float>()), "1.0");
        failures += fail("layers: image_defaults width preserved",
            merged.get_option("width").get<int>() == 512,
            std::to_string(merged.get_option("width").get<int>()), "512");
        failures += fail("layers: image_defaults sampling_method preserved",
            merged.get_option("sampling_method").get<std::string>() == "euler",
            merged.get_option("sampling_method").get<std::string>(), "euler");
        failures += fail("layers: image_defaults flow_shift preserved",
            merged.get_option("flow_shift").get<float>() == 3.0f,
            std::to_string(merged.get_option("flow_shift").get<float>()), "3.0");
    }

    // A2: model recipe_options (registry entry) override image_defaults
    {
        json model_ro = {
            {"steps", 12},
            {"cfg_scale", 3.5f},
            {"sdcpp_args", "--diffusion-fa"},
        };

        RecipeOptions merged = RecipeOptions::merge_precedence_layers(
            "sd-cpp", image_defaults, model_ro, json(nullptr));

        failures += fail("layers: model recipe_options steps override image_defaults",
            merged.get_option("steps").get<int>() == 12,
            std::to_string(merged.get_option("steps").get<int>()), "12");
        failures += fail("layers: model recipe_options cfg_scale override image_defaults",
            merged.get_option("cfg_scale").get<float>() == 3.5f,
            std::to_string(merged.get_option("cfg_scale").get<float>()), "3.5");
        failures += fail("layers: model recipe_options sdcpp_args present",
            merged.get_option("sdcpp_args").get<std::string>() == "--diffusion-fa",
            merged.get_option("sdcpp_args").get<std::string>(), "--diffusion-fa");
        failures += fail("layers: width still from image_defaults when not overridden",
            merged.get_option("width").get<int>() == 512,
            std::to_string(merged.get_option("width").get<int>()), "512");
    }

    // A3: user-saved options override both model recipe_options and image_defaults.
    // This is the "Model Options win over Image Defaults" guarantee.
    {
        json model_ro = {{"steps", 12}};
        json user_saved = {
            {"steps", 20},
            {"width", 1024},
            {"sdcpp_args", "--mmap"},
        };

        RecipeOptions merged = RecipeOptions::merge_precedence_layers(
            "sd-cpp", image_defaults, model_ro, user_saved);

        failures += fail("layers: user-saved steps beats model recipe_options",
            merged.get_option("steps").get<int>() == 20,
            std::to_string(merged.get_option("steps").get<int>()), "20");
        failures += fail("layers: user-saved width beats image_defaults",
            merged.get_option("width").get<int>() == 1024,
            std::to_string(merged.get_option("width").get<int>()), "1024");
        failures += fail("layers: cfg_scale still from image_defaults when not overridden",
            merged.get_option("cfg_scale").get<float>() == 1.0f,
            std::to_string(merged.get_option("cfg_scale").get<float>()), "1.0");
    }

    // A4: *_args keys are replaced wholesale at the model level (no token merge).
    // Token merging happens later, in inherit(), between router layers.
    {
        json model_ro = {{"sdcpp_args", "--threads 8 --mmap"}};
        json user_saved = {{"sdcpp_args", "--threads 4"}};

        RecipeOptions merged = RecipeOptions::merge_precedence_layers(
            "sd-cpp", image_defaults, model_ro, user_saved);

        std::string args = merged.get_option("sdcpp_args").get<std::string>();
        failures += fail("args: user-saved sdcpp_args fully replaces model-level",
            args == "--threads 4", args, "--threads 4");
    }

    // A5: no layers at all — empty result, get_option falls back to defaults
    {
        json null_opts = json(nullptr);
        RecipeOptions merged = RecipeOptions::merge_precedence_layers(
            "sd-cpp", null_opts, null_opts, null_opts);

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
        RecipeOptions lower("sd-cpp", image_defaults);
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
            "sd-cpp", only_steps, json(nullptr), json(nullptr));

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
            "sd-cpp", json(nullptr), json(nullptr), json(nullptr));

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

    // --- KV cache quantization config surface: four new llamacpp options ---

    // Each of the four keys resolves to its documented default when unset
    {
        RecipeOptions defaults_only("llamacpp", json::object());
        failures += fail("kv-cache: kv_cache_quantization defaults to f16",
            defaults_only.get_option("kv_cache_quantization").get<std::string>() == "f16",
            defaults_only.get_option("kv_cache_quantization").get<std::string>(), "f16");
        failures += fail("kv-cache: max_kv_quantization defaults to f16",
            defaults_only.get_option("max_kv_quantization").get<std::string>() == "f16",
            defaults_only.get_option("max_kv_quantization").get<std::string>(), "f16");
        failures += fail("kv-cache: min_kv_quantization defaults to q8_0",
            defaults_only.get_option("min_kv_quantization").get<std::string>() == "q8_0",
            defaults_only.get_option("min_kv_quantization").get<std::string>(), "q8_0");
        failures += fail("kv-cache: kv_cache_priority defaults to balanced",
            defaults_only.get_option("kv_cache_priority").get<std::string>() == "balanced",
            defaults_only.get_option("kv_cache_priority").get<std::string>(), "balanced");
    }

    // "auto" set at the saved-model layer survives merge and is readable —
    // the regression the sentinel carve-out exists to prevent.
    {
        json saved_model_ro = {{"kv_cache_quantization", "auto"}};
        RecipeOptions merged = RecipeOptions::merge_precedence_layers(
            "llamacpp", json::object(), saved_model_ro, json(nullptr));
        failures += fail("kv-cache: auto survives merge_precedence_layers",
            merged.get_option("kv_cache_quantization").get<std::string>() == "auto",
            merged.get_option("kv_cache_quantization").get<std::string>(), "auto");
    }

    // "auto" at a lower layer is overridden by an explicit q8_0 at a higher layer
    {
        json lower = {{"kv_cache_quantization", "auto"}};
        json higher = {{"kv_cache_quantization", "q8_0"}};
        RecipeOptions merged = RecipeOptions::merge_precedence_layers(
            "llamacpp", lower, higher, json(nullptr));
        failures += fail("kv-cache: explicit q8_0 overrides lower-layer auto",
            merged.get_option("kv_cache_quantization").get<std::string>() == "q8_0",
            merged.get_option("kv_cache_quantization").get<std::string>(), "q8_0");
    }

    // An unset kv_cache_priority at the model layer inherits the global
    // value rather than snapping back to the default.
    {
        json global_opts = {{"kv_cache_priority", "max_context"}};
        RecipeOptions global("llamacpp", global_opts);
        RecipeOptions model("llamacpp", json::object());
        RecipeOptions merged = model.inherit(global);
        failures += fail("kv-cache: unset kv_cache_priority inherits global value",
            merged.get_option("kv_cache_priority").get<std::string>() == "max_context",
            merged.get_option("kv_cache_priority").get<std::string>(), "max_context");
    }

    // min_kv_quantization set to q4_0 is stored, not discarded as a sentinel
    {
        json opts = {{"min_kv_quantization", "q4_0"}};
        RecipeOptions ro("llamacpp", opts);
        failures += fail("kv-cache: min_kv_quantization q4_0 is stored",
            ro.has_option("min_kv_quantization") &&
                ro.get_option("min_kv_quantization").get<std::string>() == "q4_0",
            ro.get_option("min_kv_quantization").get<std::string>(), "q4_0");
    }

    // An imported/round-tripped model definition carrying all four keys
    // retains all four — recipe_options is preserved wholesale by the import
    // allowlist, and RecipeOptions' descriptor-driven key list
    // is what makes each of the four keys survive construction rather than
    // being silently dropped as unrecognized.
    {
        json imported_recipe_options = {
            {"kv_cache_quantization", "auto"},
            {"max_kv_quantization", "f16"},
            {"min_kv_quantization", "q4_0"},
            {"kv_cache_priority", "max_context"},
        };
        RecipeOptions ro("llamacpp", imported_recipe_options);
        failures += fail("kv-cache: round-tripped kv_cache_quantization retained",
            ro.get_option("kv_cache_quantization").get<std::string>() == "auto",
            ro.get_option("kv_cache_quantization").get<std::string>(), "auto");
        failures += fail("kv-cache: round-tripped max_kv_quantization retained",
            ro.get_option("max_kv_quantization").get<std::string>() == "f16",
            ro.get_option("max_kv_quantization").get<std::string>(), "f16");
        failures += fail("kv-cache: round-tripped min_kv_quantization retained",
            ro.get_option("min_kv_quantization").get<std::string>() == "q4_0",
            ro.get_option("min_kv_quantization").get<std::string>(), "q4_0");
        failures += fail("kv-cache: round-tripped kv_cache_priority retained",
            ro.get_option("kv_cache_priority").get<std::string>() == "max_context",
            ro.get_option("kv_cache_priority").get<std::string>(), "max_context");
    }

    std::printf("\n%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

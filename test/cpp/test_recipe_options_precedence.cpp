// Standalone test for recipe options precedence.
//
// Two mechanisms implement the full ladder:
//
//   RecipeOptions::merge_precedence_layers() — model-level layers
//   (ModelManager build path):
//     user-saved recipe options > model recipe_options > model image_defaults
//
//   RecipeOptions::inherit() — server-level layers (Router):
//     per-request options > model options > architecture defaults > global config
//
// Plus per-request fields in the image-generation JSON body, which SDServer
// checks first at request time.
//
// Build with: cmake --build --preset default --target test_recipe_options_precedence
// Run with: ctest --test-dir build -R '^RecipeOptionsPrecedence$' --output-on-failure

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

    // B2: *_args keys token-merge between router layers, higher layer wins on
    // conflict; unique flags from the lower layer are added
    {
        json higher = {{"sdcpp_args", "--threads 8 --cache-type q8_0"}};
        json lower = {{"sdcpp_args", "--threads 4 --mmap"}};

        RecipeOptions a("sd-cpp", higher);
        RecipeOptions b("sd-cpp", lower);
        RecipeOptions merged = a.inherit(b);

        std::string args = merged.get_option("sdcpp_args").get<std::string>();
        failures += fail("args: higher-layer --threads 8 wins over lower --threads 4",
            args.find("--threads 8") != std::string::npos &&
            args.find("--threads 4") == std::string::npos,
            args, "--threads 8 (no --threads 4)");
        failures += fail("args: lower-layer --mmap added",
            args.find("--mmap") != std::string::npos, args, "contains --mmap");
        failures += fail("args: higher-layer --cache-type preserved",
            args.find("--cache-type q8_0") != std::string::npos, args, "contains --cache-type q8_0");
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

    std::printf("\n%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

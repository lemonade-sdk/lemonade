#include "lemon/auto_tune.h"

#include <cstdio>

namespace {

int failures = 0;

void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}

} // namespace

int main() {
    lemon::ModelInfo model;
    model.model_name = "AdaptiveWeights";
    model.recipe = "extensor";
    model.type = lemon::ModelType::LLM;
    model.device = lemon::DEVICE_GPU;
    model.size = 1e9;
    model.min_resident_gb = 1e9;

    const lemon::RecipeOptions automatic("extensor", {{"ctx_size", -1}});
    check("adaptive weights use 4096 tokens regardless of static memory estimates",
          lemon::resolve_auto_ctx_size(automatic, model) == 4096);
    check("omitted context size uses the adaptive fallback",
          lemon::resolve_auto_ctx_size(
              lemon::RecipeOptions("extensor", lemon::json::object()), model) == 4096);

    for (int size : {0, 8192, 32768}) {
        const lemon::RecipeOptions explicit_size("extensor", {{"ctx_size", size}});
        check("explicit adaptive context size needs no auto-resolution",
              lemon::resolve_auto_ctx_size(explicit_size, model) == -2);
        check("explicit context size is preserved",
              explicit_size.get_option("ctx_size") == size);
    }

    model.recipe = "llamacpp";
    check("resident weights exceeding available memory are still rejected",
          lemon::compute_auto_context_size(model, 128.0) == lemon::AUTO_CTX_NO_CAPACITY);
    model.size = 3.0;
    model.min_resident_gb = 0.0;
    check("resident models retain memory-based context sizing",
          lemon::compute_auto_context_size(model, 128.0) > 4096);
    check("explicit resident context size needs no auto-resolution",
          lemon::resolve_auto_ctx_size(
              lemon::RecipeOptions("llamacpp", {{"ctx_size", 8192}}), model) == -2);

    return failures == 0 ? 0 : 1;
}

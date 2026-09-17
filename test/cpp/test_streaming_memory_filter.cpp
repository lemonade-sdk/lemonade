// Coverage for the pure memory-fit helpers used to size-filter streaming
// backends (streams_model_from_storage). A streaming model is filtered on its
// resident working set against the device's largest memory pool, not the full
// model size against system RAM — because the model is read from disk on demand
// and can legitimately exceed memory.

#include "lemon/model_manager.h"

#include <cstdio>

using lemon::ModelManager;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

int main() {
    // working set: declared floor wins; fall back to full size when undeclared.
    check("declared min_resident is the working set",
          ModelManager::streaming_working_set_gb(16.0, 81.0) == 16.0);
    check("no declared floor falls back to full size",
          ModelManager::streaming_working_set_gb(0.0, 81.0) == 81.0);

    // fit decision against the device pool.
    check("working set within pool -> keep",
          ModelManager::streaming_model_exceeds_pool(16.0, 64.0) == false);
    check("working set equal to pool -> keep",
          ModelManager::streaming_model_exceeds_pool(64.0, 64.0) == false);
    check("working set above pool -> filter",
          ModelManager::streaming_model_exceeds_pool(96.0, 64.0) == true);

    // unknown pool (<=0) is missing data, not a reason to hide.
    check("unknown pool never filters",
          ModelManager::streaming_model_exceeds_pool(81.0, 0.0) == false);

    // end-to-end of the two helpers: an 81 GB model with a 16 GB floor fits a
    // 64 GB pool, but the same model with no declared floor does not.
    check("81 GB model with 16 GB floor fits a 64 GB pool",
          ModelManager::streaming_model_exceeds_pool(
              ModelManager::streaming_working_set_gb(16.0, 81.0), 64.0) == false);
    check("81 GB model with no floor does not fit a 64 GB pool",
          ModelManager::streaming_model_exceeds_pool(
              ModelManager::streaming_working_set_gb(0.0, 81.0), 64.0) == true);

    if (g_failures == 0) {
        std::printf("All streaming memory filter tests passed.\n");
    } else {
        std::printf("%d streaming memory filter test(s) failed.\n", g_failures);
    }
    return g_failures == 0 ? 0 : 1;
}

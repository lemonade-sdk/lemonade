// Test for lemon::backends::amica::bundle_context_length().
// Build with: cmake --build --preset default --target test_amica_bundle
// Run with: ctest --test-dir build -R amica_bundle --output-on-failure

#include "lemon/backends/amica/amica_bundle.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace fs = std::filesystem;
using lemon::backends::amica::bundle_context_length;

namespace {

std::string token_decoder(int mask, int cache) {
    const std::string kv = "!cgc.tensor<1x2x" + std::to_string(cache) + "x128x!cgc.float16>";
    return "cgc.module @m {\n"
           "  cgc.entry_point @main_graph(%arg0: !cgc.tensor<1x1x!cgc.int64>, %arg1: !cgc.tensor<1x" +
           std::to_string(mask) + "x!cgc.int64>, %arg2: " + kv + ", %arg3: " + kv + ") -> (" + kv +
           ") {\n  }\n}\n";
}

std::string embedding_decoder(int cache) {
    const std::string kv = "!cgc.tensor<1x2x" + std::to_string(cache) + "x64x!cgc.float16>";
    return "cgc.module @m {\n"
           "  cgc.entry_point @main_graph(%arg0: !cgc.tensor<1x1x896x!cgc.float32>, "
           "%arg1: !cgc.tensor<1x1x!cgc.int32>, %arg2: !cgc.tensor<1x!cgc.int32>, %arg3: " +
           kv + ", %arg4: " + kv + ") -> " + kv + " {\n  }\n}\n";
}

void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

bool expect(const char* name, const std::optional<int64_t>& actual, const std::optional<int64_t>& expected) {
    const bool ok = actual == expected;
    std::printf("%s %s: got %s, expected %s\n", ok ? "PASS" : "FAIL", name,
                actual ? std::to_string(*actual).c_str() : "none",
                expected ? std::to_string(*expected).c_str() : "none");
    return ok;
}

}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "lemon_test_amica_bundle";
    fs::remove_all(root);
    int failures = 0;

    write(root / "derived" / "decoder_bf16residual.mlir", token_decoder(2048, 2048));
    failures += !expect("derived self-contained bundle uses the mask length",
                        bundle_context_length((root / "derived").u8string()), 2048);

    write(root / "mask_wider" / "decoder_fp16act.mlir", token_decoder(193, 192));
    failures += !expect("mask length wins over a shorter KV cache",
                        bundle_context_length((root / "mask_wider").u8string()), 193);

    write(root / "embedding" / "decoder_fp16act.mlir", embedding_decoder(192));
    failures += !expect("embedding-input bundle uses the KV-cache sequence dimension",
                        bundle_context_length((root / "embedding").u8string()), 192);

    write(root / "prepared" / "src" / "decoder_named.mlir",
          "cgc.module @m {\n  cgc.entry_point @main_graph(%token_ids: !cgc.tensor<1x1x!cgc.int64>, "
          "%attention_mask: !cgc.tensor<1x2048x!cgc.int64>, %state: !cgc.tensor<1x48x128x128x!cgc.float32>) "
          "-> !cgc.tensor<1x1x248320x!cgc.float16> {\n  }\n}\n");
    write(root / "prepared" / "decoder_fp16act.mlir", token_decoder(192, 192));
    write(root / "prepared" / "amica_app.json",
          "\xEF\xBB\xBF{\"source_model_dir\": \"src\", \"graphs\": {\"decoder\": \"decoder_named.mlir\"},"
          " \"generation\": {\"self_contained_logits\": true, \"decoder_attention_mask_input\": \"arg1\"}}");
    failures += !expect("manifest decoder and mask input are honored",
                        bundle_context_length((root / "prepared").u8string()), 2048);

    fs::create_directories(root / "empty");
    failures += !expect("directory without a decoder is unknown",
                        bundle_context_length((root / "empty").u8string()), std::nullopt);

    write(root / "broken" / "decoder.mlir", "cgc.module @m {\n  cgc.entry_point @main_graph(%arg0: ");
    failures += !expect("truncated entry point is unknown",
                        bundle_context_length((root / "broken").u8string()), std::nullopt);

    write(root / "bad_manifest" / "amica_app.json", "{not json");
    write(root / "bad_manifest" / "decoder_fp16act.mlir", token_decoder(192, 192));
    failures += !expect("unreadable manifest is unknown",
                        bundle_context_length((root / "bad_manifest").u8string()), std::nullopt);

    fs::remove_all(root);
    std::printf("\n%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}

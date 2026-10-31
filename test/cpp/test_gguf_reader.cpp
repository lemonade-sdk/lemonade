// Standalone test for the GGUF KV-header reader (src/cpp/include/lemon/gguf_reader.h).

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "lemon/gguf_reader.h"

namespace {

int failures = 0;

void check(bool condition, const std::string& description) {
    if (condition) {
        std::cout << "  PASS: " << description << "\n";
    } else {
        std::cout << "  FAIL: " << description << "\n";
        ++failures;
    }
}

// A GGUF header written by hand, in the order the fields appear on disk.
class GgufWriter {
  public:
    void header(uint64_t kv_count) {
        bytes_.append("GGUF", 4);
        put(3u);
        put<uint64_t>(0);  // tensor_count: the reader never reads past the KV pairs
        put<uint64_t>(kv_count);
    }

    void string(const std::string& key, const std::string& value) {
        put_string(key);
        put<uint32_t>(8);  // STRING
        put_string(value);
    }

    void int32(const std::string& key, int32_t value) {
        put_string(key);
        put<uint32_t>(4);  // INT32
        put(value);
    }

    void float_array(const std::string& key, size_t count) {
        put_string(key);
        put<uint32_t>(9);  // ARRAY
        put<uint32_t>(6);  // FLOAT32
        put<uint64_t>(count);
        for (size_t i = 0; i < count; ++i) put(static_cast<float>(i));
    }

    void bool_array(const std::string& key, const std::vector<uint8_t>& values) {
        put_string(key);
        put<uint32_t>(9);
        put<uint32_t>(7); // BOOL
        put<uint64_t>(values.size());
        for (auto v : values) put(v);
    }

    void int_array(const std::string& key, const std::vector<int32_t>& values) {
        put_string(key);
        put<uint32_t>(9);
        put<uint32_t>(5); // INT32
        put<uint64_t>(values.size());
        for (auto v : values) put(v);
    }

    void string_array(const std::string& key, const std::vector<std::string>& values) {
        put_string(key);
        put<uint32_t>(9);  // ARRAY
        put<uint32_t>(8);  // STRING
        put<uint64_t>(values.size());
        for (const auto& value : values) put_string(value);
    }

    void write(const std::filesystem::path& path) const {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes_.data(), static_cast<std::streamsize>(bytes_.size()));
    }

  private:
    template <typename T>
    void put(T value) {
        bytes_.append(reinterpret_cast<const char*>(&value), sizeof(T));
    }

    void put_string(const std::string& value) {
        put<uint64_t>(value.size());
        bytes_.append(value);
    }

    std::string bytes_;
};

}  // namespace

int main() {
    std::cout << "=== GGUF reader tests ===\n";

    const auto dir = std::filesystem::temp_directory_path() / "lemonade_gguf_reader_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto path = dir / "model.gguf";

    // A vocabulary with entries that read like capability hints, followed by the keys
    // that do. A miscounted skip lands mid-string and loses everything after it.
    GgufWriter writer;
    writer.header(9);
    writer.string("tokenizer.ggml.tokens", "vision");
    writer.string_array("tokenizer.ggml.tokens_array", {"vision encoder", "tool_call", "image"});
    writer.float_array("tokenizer.ggml.scores", 2048);
    writer.string("general.architecture", "qwen2");
    writer.int32("qwen2.context_length", 4096);
    writer.int32("qwen2.nextn_predict_layers", 1);
    writer.string("general.name", "Qwen Vision Tune");
    writer.string("tokenizer.chat_template", "<|im_start|>tool_call ...");
    writer.int32("qwen2.block_count", 32);
    writer.write(path);

    lemon::GgufMetadata meta;
    check(lemon::read_gguf_metadata(meta, path.string()), "reads a header with arrays before the keys");
    check(meta.architecture == "qwen2", "architecture survives the skipped arrays");
    check(meta.context_length == 4096, "context length survives the skipped arrays");
    check(meta.block_count == 32, "block count after a long header is read");
    check(meta.caps.mtp, "an int32 nextn_predict_layers sets mtp");
    check(meta.caps.vision, "a hint-bearing key names the vision capability");
    check(meta.caps.tool_calling, "the chat template names the tool-calling capability");

    // Only hint-bearing keys carry capabilities: a vocabulary entry that happens to
    // read like a hint must not label the model.
    GgufWriter quiet;
    quiet.header(3);
    quiet.string_array("tokenizer.ggml.tokens", {"vision", "image", "tool_call"});
    quiet.string("general.architecture", "llama");
    quiet.int32("llama.context_length", 2048);
    quiet.write(path);

    lemon::GgufMetadata alone;
    check(lemon::read_gguf_metadata(alone, path.string()), "reads a header whose arrays carry no hints");
    check(alone.context_length == 2048, "context length read after a hint-free vocabulary");
    check(!alone.caps.vision, "vocabulary entries do not name the vision capability");
    check(!alone.caps.tool_calling, "vocabulary entries do not name the tool-calling capability");


    GgufWriter gemma;
    gemma.header(4);
    gemma.string("general.architecture", "gemma4");
    gemma.int32("gemma4.block_count", 48);
    gemma.int32("gemma4.attention.sliding_window", 1024);
    gemma.int32("gemma4.attention.key_length_swa", 256);
    gemma.write(path);
    lemon::GgufMetadata gemma_meta;
    check(lemon::read_gguf_metadata(gemma_meta, path.string()), "reads Gemma4 metadata");
    check(gemma_meta.sliding_window == 1024, "reads bounded SWA window");

    // F16 K/V can have different dimensions; explicit patterns may be stored
    // as GGUF BOOL arrays or as integer arrays of 0/1 values.
    GgufWriter full;
    full.header(12);
    full.string("general.architecture", "gemma4");
    full.int32("gemma4.block_count", 4);
    full.int32("gemma4.embedding_length", 1024);
    full.int32("gemma4.attention.head_count", 8);
    full.int_array("gemma4.attention.head_count_kv", {1, 8, 4, 2});
    full.int32("gemma4.attention.key_length", 64);
    full.int32("gemma4.attention.value_length", 128);
    full.int32("gemma4.attention.key_length_swa", 16);
    full.int32("gemma4.attention.value_length_swa", 32);
    full.int32("gemma4.attention.sliding_window", 1024);
    full.bool_array("gemma4.attention.sliding_window_pattern", {0, 1, 0, 1});
    full.int32("gemma4.attention.shared_kv_layers", 0);
    full.write(path);
    lemon::GgufMetadata full_meta;
    check(lemon::read_gguf_metadata(full_meta, path.string()),
          "GGUF bool array and variable K/V header parsed");
    check(full_meta.sliding_window_pattern == std::vector<bool>({false,true,false,true}),
          "explicit BOOL pattern is kept in layer order");
    check(full_meta.head_count_kv_per_layer == std::vector<int64_t>({1,8,4,2}),
          "variable KV heads are preserved");
    check(full_meta.head_count == 8 && full_meta.key_length == 64 &&
          full_meta.value_length == 128 && full_meta.key_length_swa == 16 &&
          full_meta.value_length_swa == 32,
          "different full and SWA K/V dimensions read exactly");

    GgufWriter numeric;
    numeric.header(4);
    numeric.string("general.architecture", "gemma3");
    numeric.int32("gemma3.block_count", 4);
    numeric.int_array("gemma3.attention.sliding_window_pattern", {1,0,1,0});
    numeric.int32("gemma3.attention.sliding_window", 1024);
    numeric.write(path);
    lemon::GgufMetadata integer_meta;
    check(lemon::read_gguf_metadata(integer_meta, path.string()) &&
          integer_meta.sliding_window_pattern_valid &&
          integer_meta.sliding_window_pattern == std::vector<bool>({true,false,true,false}),
          "GGUF 0/1 integer per-layer pattern accepted");

    GgufWriter scalar;
    scalar.header(3);
    scalar.string("general.architecture", "cohere2moe");
    scalar.int32("cohere2moe.block_count", 8);
    scalar.int32("cohere2moe.attention.sliding_window_pattern", 4);
    scalar.write(path);
    lemon::GgufMetadata scalar_meta;
    check(lemon::read_gguf_metadata(scalar_meta, path.string()) &&
          scalar_meta.sliding_window_pattern_scalar_present &&
          scalar_meta.sliding_window_pattern_period == 4,
          "GGUF scalar periodic pattern is retained for architecture phase");

    GgufWriter broken;
    broken.header(4);
    broken.string("general.architecture", "gemma3");
    broken.int32("gemma3.block_count", 4);
    broken.int_array("gemma3.attention.sliding_window_pattern", {1,2,0,1});
    broken.int32("gemma3.attention.value_length", 128);
    broken.write(path);
    lemon::GgufMetadata broken_meta;
    check(lemon::read_gguf_metadata(broken_meta, path.string()) &&
          !broken_meta.sliding_window_pattern_valid &&
          broken_meta.value_length == 128,
          "invalid array elements force fallback without losing reader alignment");

    GgufWriter explicit_zero;
    explicit_zero.header(3);
    explicit_zero.string("general.architecture", "gemma2");
    explicit_zero.int32("gemma2.block_count", 4);
    explicit_zero.int32("gemma2.attention.sliding_window", 0);
    explicit_zero.write(path);
    lemon::GgufMetadata zero_meta;
    check(lemon::read_gguf_metadata(zero_meta, path.string()) &&
          !zero_meta.bounded_swa_metadata_valid,
          "invalid explicit window does not inherit implicit Gemma2 default");

    GgufWriter duplicate;
    duplicate.header(4);
    duplicate.string("general.architecture", "gemma3");
    duplicate.int32("gemma3.block_count", 4);
    duplicate.bool_array("gemma3.attention.sliding_window_pattern", {1, 0, 1, 0});
    duplicate.bool_array("gemma3.attention.sliding_window_pattern", {1, 0, 1, 0});
    duplicate.write(path);
    lemon::GgufMetadata duplicate_meta;
    check(lemon::read_gguf_metadata(duplicate_meta, path.string()) &&
          !duplicate_meta.sliding_window_pattern_valid,
          "contradictory duplicated GGUF SWA patterns force fallback");

    GgufWriter unsupported;
    unsupported.header(4);
    unsupported.string("general.architecture", "gemma3");
    unsupported.int32("gemma3.block_count", 4);
    unsupported.float_array("gemma3.attention.sliding_window_pattern", 4);
    unsupported.int32("gemma3.attention.key_length", 128);
    unsupported.write(path);
    lemon::GgufMetadata unsupported_meta;
    check(lemon::read_gguf_metadata(unsupported_meta, path.string()) &&
          !unsupported_meta.sliding_window_pattern_valid &&
          unsupported_meta.key_length == 128,
          "unsupported GGUF pattern element type is skipped safely");

    std::filesystem::remove_all(dir);
    std::cout << (failures == 0 ? "PASSED: all checks passed\n" : "FAILED: some checks failed\n");
    return failures == 0 ? 0 : 1;
}

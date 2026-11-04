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

    std::filesystem::remove_all(dir);
    std::cout << (failures == 0 ? "PASSED: all checks passed\n" : "FAILED: some checks failed\n");
    return failures == 0 ? 0 : 1;
}

#include "lemon/auto_tune.h"
#include "lemon/backends/hf_cache_util.h"
#include "lemon/backends/install_staging.h"
#include "lemon/backends/llamacpp/llamacpp_gguf.h"
#include "lemon/backends/strata/strata.h"
#include "lemon/backends/strata/strata_model.h"
#include "lemon/backends/strata/strata_process.h"
#include "lemon/backends/strata/strata_runtime.h"
#include "lemon/backends/strata/strata_server.h"
#include "lemon/error_types.h"
#include "lemon/model_manager.h"
#include "lemon/models/store_builder.h"
#include "lemon/system_info.h"
#include "lemon/utils/custom_args.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <ggml.h>
#include <gguf.h>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using namespace lemon;
using namespace lemon::backends;
int failures = 0;

void check(const std::string& name, bool passed) {
    std::printf("%s %s\n", passed ? "PASS" : "FAIL", name.c_str());
    if (!passed) ++failures;
}

template<class F>
void rejects(const std::string& name, F fn) {
    try { fn(); check(name, false); }
    catch (const std::exception&) { check(name, true); }
}

int fixture_engine(const std::string& scenario) {
    const auto* environment = std::getenv("LEMONADE_STRATA_NATIVE_FIXTURE");
    if (!environment || std::string(environment) != "1") return 1;
    std::atomic<bool> cancelled{false};
    std::thread worker;
    if (scenario == "startup-delay") std::this_thread::sleep_for(std::chrono::seconds(10));
    std::cout << "INFO native-fixture=1\nREADY 8192 stop\n" << std::flush;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "STOP") { cancelled = true; continue; }
        if (line == "QUIT") break;
        const bool images = line.rfind("GENI ", 0) == 0;
        if (!images && line.rfind("GEN ", 0) != 0) { std::cout << "ERR invalid command\n" << std::flush; continue; }
        const auto ids = line.substr(line.rfind(' ') + 1);
        if (images) {
            std::ifstream embeddings("request.sve", std::ios::binary);
            const auto key_start = line.find("img_keys=");
            if (key_start == std::string::npos) return 4;
            std::istringstream keys(line.substr(key_start + 9, line.find(' ', key_start) - key_start - 9));
            int total_image_tokens = 0;
            while (embeddings.peek() != std::char_traits<char>::eof()) {
                std::array<int32_t, 5> header{};
                embeddings.read(reinterpret_cast<char*>(header.data()), sizeof(header));
                if (!embeddings || header[0] != 0x31455653 || header[4] != 4) return 3;
                total_image_tokens += header[1];
                std::array<char, 64> payload{};
                embeddings.read(payload.data(), payload.size());
                std::string key;
                if (!std::getline(keys, key, ',') || key != std::string(payload.data(), 16)) return 5;
            }
            std::istringstream tokens(ids);
            std::string token;
            int pads = 0;
            while (std::getline(tokens, token, ',')) if (token == "260") ++pads;
            if (pads != total_image_tokens || total_image_tokens == 0 || line.find("img_keys=") == std::string::npos) return 4;
        }
        if (worker.joinable()) worker.join();
        cancelled = false;
        std::istringstream input(line);
        std::string gen;
        int max_new;
        input >> gen >> max_new;
        const int prompt_tokens = static_cast<int>(std::count(ids.begin(), ids.end(), ',')) + 1;
        worker = std::thread([&, max_new, prompt_tokens] {
            if (scenario == "bad-token") {
                std::cout << "T -1\n" << std::flush;
                return;
            }
            const std::string text = scenario == "reasoning" ? "<think>Check</think>Hello native" :
                                     scenario == "tool" ? "<tool_call>\n<function=weather>\n<parameter=city>\nRome\n</parameter>\n</function>\n</tool_call>" :
                                     scenario == "utf8" ? "H\xc3\xa9" :
                                     "Hello native";
            const int count = scenario == "slow" ? max_new : std::min(max_new, static_cast<int>(text.size()));
            int generated = 0;
            std::cout << "PP " << prompt_tokens << " " << prompt_tokens << " 1 100\n" << std::flush;
            if (scenario == "prefill" && max_new > 3) {
                for (int i = 0; i < 1000 && !cancelled; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            for (; generated < count && !cancelled; ++generated) {
                std::cout << "T " << static_cast<int>(static_cast<unsigned char>(text[generated % text.size()])) << "\n" << std::flush;
                std::this_thread::sleep_for(std::chrono::milliseconds(scenario == "slow" ? 10 : 2));
            }
            std::cout << "DONE " << generated << " " << prompt_tokens << " 1 1 "
                      << (cancelled ? "cancel" : count == max_new ? "length" : "eos")
                      << " 0 0 " << (scenario == "cached" ? std::min(5, prompt_tokens) : 0) << "\n" << std::flush;
        });
    }
    cancelled = true;
    if (worker.joinable()) worker.join();
    return 0;
}

int fixture_vision(const std::string& scenario) {
    const auto* environment = std::getenv("LEMONADE_STRATA_NATIVE_FIXTURE");
    if (!environment || std::string(environment) != "1") return 1;
    const auto trace = utils::get_environment_variable_utf8("LEMONADE_STRATA_FIXTURE_TRACE");
    if (!trace.empty()) {
        std::ofstream(utils::path_from_utf8(trace)) << utils::path_to_utf8(fs::current_path());
    }
    if (scenario == "startup-delay") std::this_thread::sleep_for(std::chrono::seconds(10));
    if (scenario == "startup-error") { std::cout << "ERR fixture startup failure\n" << std::flush; return 2; }
    std::cout << "READY 4\n" << std::flush;
    std::string line;
    std::set<std::string> encoded;
    while (std::getline(std::cin, line)) {
        if (line == "QUIT") break;
        std::istringstream request(line);
        std::string tag, image, output;
        request >> tag >> image >> output;
        if (!request || tag != "ENC") return 3;
        std::ifstream input(image, std::ios::binary);
        const std::string data{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        if (scenario == "slow") std::this_thread::sleep_for(std::chrono::seconds(5));
        if (data == "invalid") { std::cout << "ERR unsupported image\n" << std::flush; continue; }
        if (!encoded.insert(output).second) { std::cout << "ERR image was encoded twice\n" << std::flush; continue; }
        std::ofstream file(output, std::ios::binary);
        const int32_t header[] = {0x31455653, 4, 2, 2, scenario == "bad-embedding" ? 8 : 4};
        float values[16] = {};
        std::memcpy(values, output.data(), 16);
        file.write(reinterpret_cast<const char*>(header), sizeof(header));
        file.write(reinterpret_cast<const char*>(values), sizeof(values));
        file.close();
        std::cout << "OK 4 2 2 1\n" << std::flush;
    }
    return 0;
}

std::string unicode(int cp) {
    if (cp < 128) return std::string(1, static_cast<char>(cp));
    return {static_cast<char>(0xc0 | (cp >> 6)), static_cast<char>(0x80 | (cp & 63))};
}

void tokenizer_metadata(gguf_context* file) {
    std::vector<std::string> words;
    int shifted = 256;
    for (int b = 0; b < 256; ++b) {
        const bool direct = (b >= 33 && b <= 126) || (b >= 161 && b <= 172) || b >= 174;
        words.push_back(unicode(direct ? b : shifted++));
    }
    words.insert(words.end(), {"<|endoftext|>", "<|im_start|>", "<|im_end|>",
        "<|vision_start|>", "<|image_pad|>", "<|vision_end|>"});
    std::vector<const char*> strings;
    std::vector<int32_t> types(words.size(), 1);
    for (size_t i = 0; i < words.size(); ++i) {
        strings.push_back(words[i].c_str());
        if (i >= 256) types[i] = 3;
    }
    gguf_set_val_str(file, "general.architecture", "llama");
    gguf_set_val_str(file, "tokenizer.ggml.model", "gpt2");
    gguf_set_val_str(file, "tokenizer.ggml.pre", "qwen35");
    gguf_set_arr_str(file, "tokenizer.ggml.tokens", strings.data(), strings.size());
    const char* empty_merges[] = {nullptr};
    gguf_set_arr_str(file, "tokenizer.ggml.merges", empty_merges, 0);
    gguf_set_arr_data(file, "tokenizer.ggml.token_type", GGUF_TYPE_INT32, types.data(), types.size());
    gguf_set_val_u32(file, "tokenizer.ggml.eos_token_id", 258);
    gguf_set_val_u32(file, "tokenizer.ggml.bos_token_id", 256);
    gguf_set_val_bool(file, "tokenizer.ggml.add_bos_token", false);
    gguf_set_val_bool(file, "tokenizer.ggml.add_eos_token", false);
    gguf_set_val_str(file, "tokenizer.chat_template",
        "{% if tools %}{{ '<|im_start|>system\\n' }}{{ tools | tojson }}{{ '<|im_end|>\\n' }}{% endif %}"
        "{% for message in messages %}{{ '<|im_start|>' + message['role'] + '\\n' }}"
        "{% if message.reasoning_content %}{{ '<think>' + message.reasoning_content + '</think>' }}{% endif %}"
        "{{ message.content or '' }}"
        "{% for call in message.tool_calls or [] %}{{ '<tool_call>\\n<function=' + call.function.name + '>\\n' }}"
        "{% for key, value in call.function.arguments.items() %}"
        "{{ '<parameter=' + key + '>\\n' }}{{ value if value is string else value | tojson }}{{ '\\n</parameter>\\n' }}"
        "{% endfor %}{{ '</function>\\n</tool_call>' }}{% endfor %}{{ '<|im_end|>\\n' }}"
        "{% endfor %}{% if add_generation_prompt %}{{ '<|im_start|>assistant\\n' }}{% endif %}");
}

void make_shards(const std::vector<fs::path>& paths, bool compat = false, int expert_count = 2,
                 const std::string& hadamard = "", int hadamard_block = 64) {
    const int feed_forward = hadamard.empty() ? 64 : 128;
    for (size_t i = 0; i < paths.size(); ++i) {
        auto* file = gguf_init_empty();
        auto* weights = ggml_init({12 * 1024 * 1024, nullptr, false});
        if (i == 0) tokenizer_metadata(file);
        if (i == 0 && !hadamard.empty()) {
            gguf_set_val_u32(file, "prism.hadamard.version", 1);
            gguf_set_val_u32(file, "prism.hadamard.block_size", hadamard_block);
            gguf_set_val_str(file, "prism.hadamard.transform", "normalized-sylvester-walsh-hadamard");
            gguf_set_val_str(file, "prism.hadamard.axis", "input-last-dimension");
            gguf_set_val_str(file, "prism.hadamard.sign_mode", hadamard.c_str());
            const char* names[] = {"blk.0.ffn_gate_exps.weight", "blk.0.ffn_up_exps.weight", "blk.0.ffn_down_exps.weight",
                "blk.1.ffn_gate_exps.weight", "blk.1.ffn_up_exps.weight", "blk.1.ffn_down_exps.weight"};
            gguf_set_arr_str(file, "prism.hadamard.weight_names", names, 6);
            if (hadamard == "explicit") {
                const int32_t widths[] = {128, 256};
                std::vector<int32_t> signs(384, 1);
                signs[0] = -1;
                gguf_set_arr_data(file, "prism.hadamard.sign_widths", GGUF_TYPE_INT32, widths, 2);
                gguf_set_arr_data(file, "prism.hadamard.sign_values", GGUF_TYPE_INT32, signs.data(), signs.size());
            }
        }
        const auto add = [&](const std::string& name, ggml_type type, int64_t x, int64_t y, int64_t z) {
            auto* t = ggml_new_tensor_3d(weights, type, x, y, z);
            ggml_set_name(t, name.c_str());
            std::memset(t->data, 0, ggml_nbytes(t));
            if (type == GGML_TYPE_F32) std::fill_n(static_cast<float*>(t->data), ggml_nelements(t), 1.0f);
            if (type == GGML_TYPE_F16 || type == GGML_TYPE_BF16) {
                std::fill_n(static_cast<uint16_t*>(t->data), ggml_nelements(t),
                    static_cast<uint16_t>(type == GGML_TYPE_F16 ? 0x3c00 : 0x3f80));
            }
            if (compat && name == "blk.0.ssm_alpha.weight") {
                const float values[] = {1.00390625f, 1.01171875f, -0.25f, 1.0f};
                std::memcpy(t->data, values, sizeof(values));
            }
            if (compat && type == GGML_TYPE_Q8_0) {
                for (size_t at = 0; at < ggml_nbytes(t); at += 34) {
                    const uint16_t scale = 0x3c00;
                    std::memcpy(static_cast<char*>(t->data) + at, &scale, 2);
                    std::memset(static_cast<char*>(t->data) + at + 2, 1, 32);
                }
            }
            gguf_add_tensor(file, t);
        };
        for (int layer = 0; layer < 2; ++layer) {
            const std::string prefix = "blk." + std::to_string(layer) + ".";
            if (i == 0) {
                add(prefix + "ffn_gate_exps.weight", GGML_TYPE_Q2_0, 256, feed_forward, expert_count);
                add(prefix + "ffn_up_exps.weight", GGML_TYPE_Q2_0, 256, feed_forward, expert_count);
                add(prefix + "ffn_gate_inp.weight", GGML_TYPE_BF16, 256, expert_count, 1);
                add(prefix + "attn_norm.weight", GGML_TYPE_F32, 256, 1, 1);
            } else if (i == paths.size() - 1) {
                add(prefix + "ffn_down_exps.weight", GGML_TYPE_IQ4_NL, feed_forward, 256, expert_count);
            }
        }
        if (i == 0) {
            add("token_embd.weight", GGML_TYPE_Q8_0, 256, 256, 1);
            add("blk.1.ple_key.weight", GGML_TYPE_Q2_0, 256, 256, 1);
            add("blk.0.ssm_alpha.weight", GGML_TYPE_F32, 4, 1, 1);
            add("blk.0.ssm_norm.weight", compat ? GGML_TYPE_Q8_0 : GGML_TYPE_F16, compat ? 32 : 4, 1, 1);
            add("blk.0.ple_conv1d.weight", compat ? GGML_TYPE_BF16 : GGML_TYPE_F32, 4, 1, 1);
            if (compat) add("blk.0.hc_attn_down.weight", GGML_TYPE_Q8_0, 32, 2, 1);
        } else if (i == paths.size() - 1) add("per_layer_token_embd.weight", GGML_TYPE_Q8_0, 256, 32, 1);
        else add("aux." + std::to_string(i) + ".weight", GGML_TYPE_F32, 4, 1, 1);
        check("Write synthetic GGUF shard", gguf_write_to_file(file, utils::path_to_utf8(paths[i]).c_str(), false));
        gguf_free(file);
        ggml_free(weights);
    }
}

void gyro_types(const fs::path& path, uint32_t gu = 145, uint32_t down = 146) {
    auto* file = gguf_init_from_file(utils::path_to_utf8(path).c_str(), {true, nullptr});
    std::string header(gguf_get_meta_size(file), '\0');
    gguf_get_meta_data(file, header.data());
    std::fstream output(path, std::ios::binary | std::ios::in | std::ios::out);
    for (int64_t i = 0; i < gguf_get_n_tensors(file); ++i) {
        const std::string name = gguf_get_tensor_name(file, i);
        if (name.find("_exps.weight") == std::string::npos) continue;
        const auto at = header.rfind(name) + name.size();
        uint32_t dims;
        std::memcpy(&dims, header.data() + at, 4);
        const auto type = name.find("down_exps") == std::string::npos ? gu : down;
        output.seekp(static_cast<std::streamoff>(at + 4 + dims * 8));
        output.write(reinterpret_cast<const char*>(&type), 4);
    }
    gguf_free(file);
}

void test_model_formats(const fs::path& root) {
    for (const int count : {3, 4, 6}) {
        const auto folder = root / ("compat-" + std::to_string(count));
        fs::create_directories(folder);
        std::vector<fs::path> shards;
        for (int i = 0; i < count; ++i) shards.push_back(folder / (std::to_string(i) + ".gguf"));
        make_shards(shards, true);
        const auto expert_bytes = strata::model_expert_bytes(shards);
        check("Memory estimates count actual expert tensor bytes across shards",
            expert_bytes == 4 * ggml_row_size(GGML_TYPE_Q2_0, 256) * 64 * 2 +
                2 * ggml_row_size(GGML_TYPE_IQ4_NL, 64) * 256 * 2);
        const auto pack = folder / "pack";
        rejects("Strict packs reject quantized required dense forms", [&] { strata::prepare_pack(shards, pack); });
        strata::prepare_pack(shards, pack, nullptr, true);
        check("Compat packs support every shard count", strata::pack_present(pack));
        std::ifstream index(pack / "index.txt");
        std::ifstream dense(pack / "dense.bin", std::ios::binary);
        std::string line;
        while (std::getline(index, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream row(line);
            std::string name;
            int source, kind;
            uint64_t offset, bytes;
            row >> name >> source >> kind >> offset >> bytes;
            dense.seekg(static_cast<std::streamoff>(offset));
            if (name == "blk.0.ssm_alpha.weight") {
                uint16_t values[4]{};
                dense.read(reinterpret_cast<char*>(values), sizeof(values));
                check("BF16 rounding is nearest-even at both halfway ties",
                    kind == 4 && bytes == 8 && values[0] == 0x3f80 && values[1] == 0x3f82 && values[2] == 0xbe80);
            } else if (name == "blk.0.hc_attn_down.weight") {
                std::vector<uint16_t> values(64);
                dense.read(reinterpret_cast<char*>(values.data()), 128);
                check("Q8_0 projections dequantize into exact BF16 rows",
                    kind == 4 && bytes == 128 && std::all_of(values.begin(), values.end(), [](auto v) { return v == 0x3f80; }));
            } else if (name == "blk.0.ssm_norm.weight") {
                float value = 0;
                dense.read(reinterpret_cast<char*>(&value), 4);
                check("Quantized required norms widen to F32", kind == 2 && bytes == 128 && value == 1.0f);
            } else if (name == "blk.0.ple_conv1d.weight") {
                uint16_t value = 0;
                dense.read(reinterpret_cast<char*>(&value), 2);
                check("BF16 convolutions convert to actual F16", kind == 5 && bytes == 8 && value == 0x3c00);
            }
        }
        const auto [native, ple] = strata::prepare_metal_native(shards, folder / "metal.gguf");
        auto* combined = gguf_init_from_file(utils::path_to_utf8(native).c_str(), {true, nullptr});
        check("Metal repacks all non-PLE tensors with page alignment", combined &&
            gguf_get_alignment(combined) == 16384 && gguf_find_key(combined, "split.count") < 0 &&
            gguf_find_tensor(combined, "per_layer_token_embd.weight") < 0 &&
            gguf_find_tensor(combined, "blk.1.ffn_down_exps.weight") >= 0 && ple == shards.back());
        for (const auto& shard : shards) {
            auto* original = gguf_init_from_file(utils::path_to_utf8(shard).c_str(), {true, nullptr});
            std::ifstream input(shard, std::ios::binary), output(native, std::ios::binary);
            std::string before_metadata(gguf_get_data_offset(original), '\0'), after_metadata(gguf_get_data_offset(combined), '\0');
            input.read(before_metadata.data(), static_cast<std::streamsize>(before_metadata.size()));
            output.read(after_metadata.data(), static_cast<std::streamsize>(after_metadata.size()));
            bool identical = true;
            for (int64_t i = 0; i < gguf_get_n_tensors(original); ++i) {
                const std::string name = gguf_get_tensor_name(original, i);
                if (name == "per_layer_token_embd.weight") continue;
                const auto target = gguf_find_tensor(combined, name.c_str());
                if (target < 0) { identical = false; continue; }
                const auto before_at = before_metadata.rfind(name) + name.size();
                const auto after_at = after_metadata.rfind(name) + name.size();
                uint32_t dimensions;
                std::memcpy(&dimensions, before_metadata.data() + before_at, 4);
                identical &= std::memcmp(before_metadata.data() + before_at, after_metadata.data() + after_at,
                    8 + dimensions * 8) == 0;
                const auto bytes = gguf_get_tensor_size(original, i);
                std::vector<char> before(bytes), after(bytes);
                input.seekg(static_cast<std::streamoff>(gguf_get_data_offset(original) + gguf_get_tensor_offset(original, i)));
                output.seekg(static_cast<std::streamoff>(gguf_get_data_offset(combined) + gguf_get_tensor_offset(combined, target)));
                input.read(before.data(), static_cast<std::streamsize>(bytes));
                output.read(after.data(), static_cast<std::streamsize>(bytes));
                identical &= input && output && before == after;
            }
            gguf_free(original);
            check("Metal repack preserves tensor shapes, types and every byte", identical);
        }
        gguf_free(combined);
        check("Metal repack cache is reusable", strata::prepare_metal_native(shards, folder / "metal.gguf").first == native);
    }
    const auto folder = root / "gyro";
    fs::create_directories(folder);
    const std::vector<fs::path> shards = {folder / "a.gguf", folder / "b.gguf"};
    for (const auto& mode : {"identity", "explicit"}) {
        make_shards(shards, true, 2, mode);
        const bool gyro_s = std::string(mode) == "identity";
        gyro_types(shards[0], gyro_s ? 144 : 145, gyro_s ? 144 : 146);
        gyro_types(shards[1], gyro_s ? 144 : 145, gyro_s ? 144 : 146);
        strata::prepare_pack(shards, folder / "pack", nullptr, true);
        std::ifstream layout(folder / "pack" / "native_experts.txt");
        std::string header, first;
        std::getline(layout, header); std::getline(layout, first);
        std::ifstream rotation(folder / "pack" / "hadamard.txt");
        const std::string text(std::istreambuf_iterator<char>(rotation), {});
        check("Gyro custom types produce v5 expert geometry", header.find("v5:") != std::string::npos &&
            first.find(gyro_s ? "0 144 144 0 26112 " : "0 145 146 0 20992 ") == 0);
        check("Hadamard input rotations are retained", text.find("block 64\n") != std::string::npos &&
            text.find("layer 0 1 1 1\n") != std::string::npos);
        if (std::string(mode) == "explicit") {
            check("Hadamard explicit sign vectors are retained", text.find("signs 128 -1 1") != std::string::npos);
        }
        rotation.close();
        fs::remove(folder / "pack" / "hadamard.txt");
        check("Missing Hadamard sidecar invalidates a prepared pack", !strata::pack_present(folder / "pack"));
    }
    make_shards(shards, false, 2, "unsupported");
    gyro_types(shards[0]); gyro_types(shards[1]);
    rejects("Unknown Hadamard sign modes fail explicitly", [&] { strata::prepare_pack(shards, folder / "pack"); });
    make_shards(shards, false, 2, "identity");
    rejects("Folded Q2_0 experts are refused", [&] { strata::prepare_pack(shards, folder / "pack"); });
    make_shards(shards, false, 256);
    strata::prepare_pack(shards, folder / "pack");
    std::ifstream pruned(folder / "pack" / "native_experts.txt");
    std::string header;
    std::getline(pruned, header);
    check("Coder-sized expert counts are not hardcoded to 512", header.find("(n_expert 256)") != std::string::npos);
    pruned.close();
    make_shards(shards, true, 256, "explicit", 128);
    gyro_types(shards[0], 144, 144);
    gyro_types(shards[1], 144, 144);
    strata::prepare_pack(shards, folder / "pack", nullptr, true);
    std::ifstream gyro_coder_layout(folder / "pack" / "native_experts.txt");
    std::string first;
    std::getline(gyro_coder_layout, header);
    std::getline(gyro_coder_layout, first);
    std::ifstream gyro_coder_rotation(folder / "pack" / "hadamard.txt");
    const std::string rotation(std::istreambuf_iterator<char>(gyro_coder_rotation), {});
    check("Gyro-Coder combines 256 experts with TQ2_T and explicit block-128 rotations",
        header.find("v5:") != std::string::npos && header.find("(n_expert 256)") != std::string::npos &&
        first.find("0 144 144 ") == 0 && rotation.find("block 128\n") != std::string::npos &&
        rotation.find("signs 128 -1 1") != std::string::npos &&
        rotation.find("signs 256 1 1") != std::string::npos && strata::pack_present(folder / "pack"));
    gyro_coder_layout.close();
    gyro_coder_rotation.close();

    const auto direct = root / "direct";
    fs::create_directories(direct);
    fs::copy_file(folder / "pack" / "tokenizer.gguf", direct / "model.gguf");
    strata::prepare_vocabulary(direct / "model.gguf", direct / "pack" / "tokenizer.gguf");
    strata::finish_preparation({direct / "model.gguf"}, direct / "pack", "native-gguf");
    check("Direct native GGUF requires no dense pack or profile", strata::pack_present(direct / "pack") &&
        !fs::exists(direct / "pack" / "dense.bin") && !fs::exists(direct / "expert-profile.bin"));
    const auto ready = root / "ready";
    fs::create_directories(ready / "pack");
    std::vector<fs::path> sources;
    for (const auto& name : {"dense.bin", "experts.bin", "index.txt", "native_experts.txt"}) {
        const auto path = ready / "pack" / name;
        std::ofstream(path) << "prebuilt-" << name;
        sources.push_back(path);
    }
    fs::copy_file(direct / "model.gguf", ready / "native.gguf");
    sources.push_back(ready / "native.gguf");
    strata::prepare_vocabulary(ready / "native.gguf", ready / "pack" / "tokenizer.gguf");
    strata::finish_preparation(sources, ready / "pack", "ready-pack");
    check("Ready packs preserve nested download paths", strata::pack_present(ready / "pack"));
    fs::remove(ready / "pack" / "experts.bin");
    check("Missing ready-pack experts invalidate completeness", !strata::pack_present(ready / "pack"));
}

class MockStrataServer : public StrataServer {
public:
    MockStrataServer() : StrataServer("info", nullptr, nullptr) {
        const auto handle = [this](const httplib::Request& req, httplib::Response& res) {
            const auto body = json::parse(req.body);
            if (body.value("stream", false)) {
                res.set_chunked_content_provider("text/event-stream",
                    [this, path = req.path, payload = req.body](size_t, httplib::DataSink& sink) {
                        forward_streaming_request(path, payload, sink);
                        return true;
                    });
            } else {
                RequestCancelScope scope({nullptr, req.is_connection_closed});
                const auto result = req.path == "/v1/responses" ? responses(body) : chat_completion(body);
                if (result.contains("error")) {
                    res.status = result["error"].value("type", "") == ErrorType::INVALID_REQUEST ? 400 : 502;
                }
                res.set_content(result.dump(), "application/json");
            }
        };
        frontend_.Post("/v1/chat/completions", handle);
        frontend_.Post("/v1/responses", handle);
    }
    ~MockStrataServer() override { unload(); }
    void load_fixture(const fs::path& vocab, const fs::path& exe, const std::string& scenario = "",
                      const std::optional<std::string>& vision_scenario = std::nullopt, const fs::path& trace = {}) {
        ServerCommand command;
        command.program = utils::path_to_utf8(exe);
        command.args = {"--engine-fixture", scenario};
        command.env = {{"LEMONADE_STRATA_NATIVE_FIXTURE", "1"}};
        std::optional<strata::VisionCommand> vision;
        if (vision_scenario) {
            auto encoder = command;
            encoder.args = {"--vision-fixture", *vision_scenario};
            if (!trace.empty()) encoder.env.emplace_back("LEMONADE_STRATA_FIXTURE_TRACE", utils::path_to_utf8(trace));
            vision = strata::VisionCommand{encoder, 300};
        }
        start_server(std::make_unique<strata::StrataProcess>(vocab, "test-model", std::move(vision)), command, 10);
        check("Native Strata has no HTTP backend port or URL", get_backend_port() == 0 && get_address().empty());
        port = frontend_.bind_to_any_port("127.0.0.1");
        if (port < 0) throw std::runtime_error("Could not bind test frontend");
        listener_ = std::thread([this] { frontend_.listen_after_bind(); });
    }
    void unload() override {
        StrataServer::unload();
        frontend_.stop();
        if (listener_.joinable()) listener_.join();
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port); }

private:
    httplib::Server frontend_;
    std::thread listener_;
    int port = 0;
};

void test_pack_layout(const std::vector<fs::path>& shards, const fs::path& pack) {
    std::ifstream index(pack / "index.txt"), dense(pack / "dense.bin", std::ios::binary);
    std::string line;
    uint64_t disk = 0, pool = 0;
    bool valid = true;
    while (std::getline(index, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line);
        std::vector<std::string> fields{std::istream_iterator<std::string>(row), std::istream_iterator<std::string>()};
        if (fields.size() != 19) { valid = false; continue; }
        const auto kind = std::stoi(fields[2]);
        const auto source_offset = std::stoull(fields[3]), source_bytes = std::stoull(fields[4]);
        const auto destination_offset = std::stoull(fields[5]), destination_bytes = std::stoull(fields[6]);
        valid &= destination_offset == pool;
        if (kind == 0) {
            valid &= source_offset == 0 && source_bytes == 0 && destination_bytes == 0 && fields[9] == "8";
            continue;
        }
        valid &= source_offset == disk && disk % 64 == 0 && pool % 64 == 0;
        dense.seekg(static_cast<std::streamoff>(disk));
        if (fields[0] == "blk.0.ssm_alpha.weight") {
            uint16_t value = 0;
            dense.read(reinterpret_cast<char*>(&value), 2);
            check("Exact F32 to BF16 dense conversion", kind == 4 && value == 0x3f80 && source_bytes == 8 && destination_bytes == 8);
        } else if (fields[0] == "blk.0.ssm_norm.weight" || fields[0] == "blk.0.ple_conv1d.weight") {
            float value = 0;
            dense.read(reinterpret_cast<char*>(&value), 4);
            check("Dense widening and engine narrowing layout", value == 1.0f && source_bytes == 16 &&
                (fields[0] == "blk.0.ssm_norm.weight" ? kind == 2 && destination_bytes == 16 : kind == 3 && destination_bytes == 8));
        }
        disk += (source_bytes + 63) / 64 * 64;
        pool += (destination_bytes + 63) / 64 * 64;
    }
    check("Every index row has 19 fields and aligned offsets", valid && disk == fs::file_size(pack / "dense.bin"));
    auto* first = gguf_init_from_file(utils::path_to_utf8(shards[0]).c_str(), {true, nullptr});
    auto* second = gguf_init_from_file(utils::path_to_utf8(shards[1]).c_str(), {true, nullptr});
    std::ifstream experts(pack / "native_experts.txt");
    std::getline(experts, line);
    check("Expert count is recorded", line.find("(n_expert 2)") != std::string::npos);
    const auto blob = ggml_row_size(GGML_TYPE_Q2_0, 256) * 64 * 2 + ggml_row_size(GGML_TYPE_IQ4_NL, 64) * 256;
    int count = 0;
    while (std::getline(experts, line)) {
        std::istringstream row(line);
        int layer, gate_type, down_type;
        uint64_t offset, bytes, gate, up, down;
        std::string names;
        row >> layer >> gate_type >> down_type >> offset >> bytes >> gate >> up >> down >> names;
        const auto prefix = "blk." + std::to_string(layer) + ".ffn_";
        const auto gguf_offset = [&](gguf_context* file, const char* role) {
            const auto tensor = gguf_find_tensor(file, (prefix + role + "_exps.weight").c_str());
            return gguf_get_data_offset(file) + gguf_get_tensor_offset(file, tensor);
        };
        check("Native expert offsets, formats and per-role shards", row && layer == count &&
            gate_type == GGML_TYPE_Q2_0 && down_type == GGML_TYPE_IQ4_NL && bytes == blob &&
            offset == count * blob * 2 && gate == gguf_offset(first, "gate") && up == gguf_offset(first, "up") &&
            down == gguf_offset(second, "down") && names ==
                shards[0].filename().string() + "," + shards[0].filename().string() + "," + shards[1].filename().string());
        ++count;
    }
    check("All expert layers are present", count == 2);
    gguf_free(first);
    gguf_free(second);
}

std::vector<json> response_stream(MockStrataServer& server, json request) {
    request["stream"] = true;
    std::string sse;
    const auto result = utils::HttpClient::post_stream(server.url() + "/v1/responses", request.dump(),
        [&](const char* data, size_t size) { sse.append(data, size); return true; },
        {{"Content-Type", "application/json"}}, 10, nullptr, utils::HttpSecurityPolicy::TrustedLoopback);
    if (result.status_code != 200) throw std::runtime_error("Responses stream failed: " + sse);
    std::vector<json> events;
    std::istringstream input(sse);
    std::string line;
    while (std::getline(input, line)) {
        if (line.rfind("data: {", 0) == 0) {
            auto event = json::parse(line.substr(6));
            if (event.contains("error")) throw std::runtime_error(event.dump());
            events.push_back(std::move(event));
        }
    }
    return events;
}

void check_response_stream(const std::vector<json>& events) {
    json items = json::array();
    std::vector<std::string> text;
    std::vector<bool> parts;
    bool valid = !events.empty();
    for (size_t i = 0; i < events.size(); ++i) {
        const auto& e = events[i];
        valid &= e.at("sequence_number") == i;
        const auto type = e.at("type").get<std::string>();
        if (type == "response.output_item.added") {
            valid &= e.at("output_index") == items.size();
            items.push_back(e.at("item"));
            text.emplace_back();
            parts.push_back(false);
        } else if (e.contains("output_index")) {
            const auto index = e.at("output_index").get<size_t>();
            if (index >= items.size()) { valid = false; continue; }
            if (e.contains("item_id")) valid &= e["item_id"] == items[index]["id"];
            if (type == "response.content_part.added" || type == "response.reasoning_summary_part.added") parts[index] = true;
            if (type == "response.output_text.delta" || type == "response.reasoning_summary_text.delta") {
                valid &= parts[index];
                text[index] += e.at("delta").get<std::string>();
            } else if (type == "response.function_call_arguments.delta") {
                text[index] += e.at("delta").get<std::string>();
            } else if (type == "response.output_text.done" || type == "response.reasoning_summary_text.done") {
                valid &= e.at("text") == text[index];
            } else if (type == "response.function_call_arguments.done") {
                valid &= e.at("arguments") == text[index];
            } else if (type == "response.output_item.done") {
                valid &= e.at("item").at("id") == items[index]["id"];
                items[index] = e.at("item");
            }
        }
    }
    valid &= !events.empty() && (events.back().value("type", "") == "response.completed" ||
                                events.back().value("type", "") == "response.incomplete");
    if (valid) valid &= events.back().at("response").at("output") == items;
    check("Responses event indices, deltas and final output agree", valid);
}

void test_model_context() {
    const auto models = utils::JsonUtils::load_from_file(utils::get_resource_path("resources/server_models.json"));
    int64_t context_length = 262144;
    for (const auto& [name, model] : models.items()) {
        if (model.value("recipe", "") != "strata") continue;
        ModelInfo info;
        info.model_name = name;
        info.recipe = "strata";
        info.checkpoints["main"] = model.at("checkpoint").get<std::string>();
        const auto checkpoint = strata::model_checkpoint(info.checkpoint());
        const auto folder = checkpoint.directory();
        const auto source = checkpoint.local_file(folder, checkpoint.native_file);
        fs::create_directories(source.parent_path());
        auto* metadata = gguf_init_empty();
        gguf_set_val_str(metadata, "general.architecture", "qwen35moe");
        gguf_set_val_u32(metadata, "qwen35moe.context_length", static_cast<uint32_t>(context_length));
        check("Write original GGUF metadata: " + name,
            gguf_write_to_file(metadata, utils::path_to_utf8(source).c_str(), true));
        gguf_free(metadata);
        fs::create_directories(folder / "pack");
        auto* tokenizer = gguf_init_empty();
        gguf_set_val_str(tokenizer, "general.architecture", "llama");
        gguf_set_val_u32(tokenizer, "llama.context_length", 131072);
        check("Write rewritten tokenizer metadata: " + name,
            gguf_write_to_file(tokenizer, utils::path_to_utf8(folder / "pack" / "tokenizer.gguf").c_str(), true));
        gguf_free(tokenizer);
        strata::ops()->populate_metadata(info, {});
        check("Original model context is exposed: " + name,
            info.max_context_window == context_length && info.gguf.architecture == "qwen35moe");
        context_length += 1024;
    }
}

void test_memory() {
    const auto catalog = utils::JsonUtils::load_from_file(utils::get_resource_path("resources/server_models.json"));
    for (const auto& [name, entry] : catalog.items()) {
        if (entry.value("recipe", "") == "strata") {
            check("Curated Strata selection is visible in the UI: " + name, entry.value("suggested", false));
        }
    }
    const auto model_info = [&](const std::string& name) {
        const auto& entry = catalog.at(name);
        ModelInfo info;
        info.recipe = entry.at("recipe").get<std::string>();
        info.checkpoints["main"] = entry.at("checkpoint").get<std::string>();
        info.size = entry.at("size").get<double>();
        info.min_resident_gb = entry.at("min_resident_gb").get<double>();
        return info;
    };
    const auto flash = model_info("Qwen3.8-Flash-Next-Q2_0-Strata");
    const auto qwen = model_info("Qwen3.6-35B-A3B-Q8_0-Strata");
    const auto* ops = strata::ops();
    const auto fits = [&](const ModelInfo& info, double ram, double device, double dedicated) {
        const json hardware = {{"amd_gpu", json::array({
            {{"available", true}, {"integrated", dedicated == 0.0}, {"vram_gb", dedicated}}
        })}};
        models::BackendAvailability availability;
        availability.system_ram_gb = ram;
        availability.largest_mem_pool_gb = device;
        availability.recipes["strata"].streams_model_from_storage = true;
        availability.models["builtin.strata-fixture"].streaming_memory_pool_gb =
            ops->streaming_memory_pool_gb(info, ram, device, hardware);
        return models::filter_hardware({{"builtin.strata-fixture", info}}, availability)
            .visible.count("builtin.strata-fixture") != 0;
    };
    check("Flash-Next requires 32 GB combined memory", flash.min_resident_gb == 32.0);
    check("RX 9070 XT and 64 GB RAM retain Flash-Next", fits(flash, 64.0, 15.8, 15.8));
    check("RAM plus dedicated VRAM meet the exact floor", fits(flash, 20.0, 12.0, 12.0));
    check("Insufficient combined memory is filtered", !fits(flash, 16.0, 12.0, 12.0));
    check("Unified memory meets the exact floor", fits(flash, 32.0, 32.0, 0.0));
    check("Unified memory is not double-counted", !fits(flash, 16.0, 16.0, 0.0));
    check("Qwen3.6 retains its system-RAM requirement", !fits(qwen, 32.0, 24.0, 24.0));
    check("64 GB RAM retains Qwen3.6 on a 16 GB GPU", fits(qwen, 64.0, 15.8, 15.8));
    check("Unknown RAM does not hide models", fits(flash, 0.0, 15.8, 15.8));
    check("Other backends keep the device-only memory budget",
        default_backend_ops()->streaming_memory_pool_gb(flash, 64.0, 15.8, json::object()) == 15.8);
    const json mixed_hardware = {
        {"amd_gpu", json::array({
            {{"available", true}, {"integrated", true}, {"vram_gb", 64.0}},
            {{"available", true}, {"integrated", false}, {"vram_gb", 12.0}},
            {{"available", false}, {"integrated", false}, {"vram_gb", 128.0}}
        })},
        {"nvidia_gpu", {{"available", true}, {"vram_gb", 24.0}}},
        {"amd_npu", {{"available", true}, {"vram_gb", 1024.0}}}
    };
    check("Combined memory counts only the largest available dedicated GPU",
        ops->streaming_memory_pool_gb(flash, 20.0, 24.0, mixed_hardware) == 44.0);
    check("AMD memory without a dedicated-device classification is not double-counted",
        ops->streaming_memory_pool_gb(flash, 20.0, 16.0,
            {{"amd_gpu", {{"available", true}, {"vram_gb", 16.0}}}}) == 20.0);
    const auto release = strata::engine_release();
    const RecipeOptions automatic("strata", {{"strata_backend", "rocm"}, {"ctx_size", -1}});
    check("Strata automatic context uses engine metadata, not full-model VRAM",
        resolve_auto_ctx_size(automatic, flash) == release.default_context);
    auto limited = flash;
    limited.max_context_window = release.default_context / 2;
    check("Strata automatic context respects the model context limit",
        resolve_auto_ctx_size(automatic, limited) == limited.max_context_window);
    const RecipeOptions explicit_context("strata", {{"strata_backend", "rocm"}, {"ctx_size", 4096}});
    check("Explicit Strata context is not replaced",
        resolve_auto_ctx_size(explicit_context, flash) == -2);
    check("Other backends retain the generic automatic context estimator",
        !default_backend_ops()->auto_context_size(flash, automatic).has_value());
}

void test_assets() {
    const auto metadata = utils::JsonUtils::load_from_file(utils::get_resource_path("resources/backend_versions.json"));
    const auto tag = metadata["strata"]["cuda"].get<std::string>();
    const auto release = strata::engine_release();
    check("Strata requires verified GitHub asset discovery", strata::spec()->require_asset_digest);
    const auto current = StrataServer::get_install_params("cuda", tag);
    const auto future = StrataServer::get_install_params("cuda", "v9.8.7");
    check("Engine downloads use the upstream release repository",
        current.repo == "Niko1221/Strata");
    check("An unseen release tag uses the same stable install metadata",
        current.repo == future.repo && current.filename == future.filename &&
        future.filename.find('*') != std::string::npos);
    const auto asset = [&](const std::string& os, const std::string& backend, const std::string& gpu = "") {
        return release.asset(os, os == "macos" ? "arm64" : "x86_64", backend, gpu);
    };
    check("Windows CUDA12", asset("windows", "cuda").find("windows-x86_64-cuda-cuda12") != std::string::npos);
    check("Linux CUDA13", asset("linux", "cuda13").find("linux-x86_64-cuda-cuda13") != std::string::npos);
    check("Metal", asset("macos", "metal").find("macos-arm64-metal") != std::string::npos);
    for (const auto& [backend, pin] : metadata.at("strata").items()) {
        check("Backend release tag remains independently pinned", pin.is_string() && !pin.get<std::string>().empty());
        const auto entry = strata::engine_release();
        for (const auto& [platform, families] : entry.targets) {
            if (!families.count(backend)) continue;
            const auto split = platform.find('-');
            for (const auto& [gpu, variant] : families.at(backend)) {
                const auto filename = entry.asset(platform.substr(0, split), platform.substr(split + 1),
                    backend, gpu == "default" ? "" : gpu);
                auto published = filename;
                published.replace(published.find('*'), 1, "9.8.7");
                const std::string digest = "sha256:" + std::string(64, 'a');
                const json github_release = {{"assets", json::array({{{"name", published}, {"digest", digest}}})}};
                const auto resolved = BackendUtils::select_release_asset(github_release, filename, true);
                check("Engine versions and checksums are resolved from the pinned release",
                    resolved.name == published && resolved.digest == digest);
            }
        }
    }
    for (const auto& os : {"windows", "linux"}) {
        for (const auto& arch : {"gfx1030", "gfx1031", "gfx1100", "gfx1101", "gfx1102", "gfx1151", "gfx1200", "gfx1201"}) {
            check(std::string(os) + "/" + arch, !asset(os, "rocm", arch).empty());
        }
    }
    for (const auto& [family, arch] : std::map<std::string, std::string>{
             {"gfx103X", "gfx1030"}, {"gfx110X", "gfx1100"}, {"gfx120X", "gfx1201"}}) {
        check("Detected AMD family is supported: " + family,
            SystemInfo::backend_supports_arch("strata", "rocm", family));
        for (const auto& os : {"windows", "linux"}) {
            check(std::string(os) + "/" + family + " selects the matching engine",
                asset(os, "rocm", family) == asset(os, "rocm", arch));
        }
    }
    check("Unsupported AMD family remains unsupported",
        !SystemInfo::backend_supports_arch("strata", "rocm", "gfx942"));
    rejects("Reject Intel Mac", [&] { release.asset("macos", "x86_64", "metal"); });
    rejects("Reject Windows ARM", [&] { release.asset("windows", "arm64", "cuda"); });
    rejects("Reject unknown AMD target", [&] { asset("linux", "rocm", "gfx942"); });
    rejects("Reject CUDA on Mac", [&] { asset("macos", "cuda"); });
    rejects("Reject arbitrary GGUF", [] { strata::model_checkpoint("other/model:IQ2_XS"); });
    rejects("Reject checkpoint traversal", [] {
        strata::model_checkpoint("ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF:../../data");
    });
    RecipeOptions options("strata", {{"strata_backend", "cuda"}});
    check("Backend selection is wired", options.get_option("strata_backend") == "cuda");
    for (const auto& support : strata::descriptor.support) {
        check("Backend format is native", support.format == BackendFormat::Native);
        if (support.backend == "rocm") {
            for (const auto& os : support.supported_os) {
                for (const auto& family : support.devices.at("amd_gpu")) {
                    check("Advertised AMD target has an official engine: " + os + "/" + family,
                        !asset(os, support.backend, family).empty());
                }
            }
        }
    }
}

void test_release_assets() {
    const std::string pattern = "strata-engine-*-windows-x86_64-cuda-cuda12.tar.gz";
    const std::string digest = "sha256:" + std::string(64, 'a');
    const json published = {{"name", "strata-engine-9.8.7-windows-x86_64-cuda-cuda12.tar.gz"}, {"digest", digest}};
    const json release = {{"assets", json::array({
        {{"name", "strata-app-windows.zip"}},
        published,
        {{"name", "strata-engine-9.8.7-windows-x86_64-cuda-cuda13.tar.gz"}, {"digest", digest}}
    })}};
    const auto selected = BackendUtils::select_release_asset(release, pattern, true);
    check("Discover exact GPU asset and SHA-256 without an engine version pin",
        selected.name == published.at("name") && selected.digest == digest);
    const auto invalid = [&](const std::string& name, const json& input, const std::string& glob = "") {
        rejects(name, [&] { BackendUtils::select_release_asset(input, glob.empty() ? pattern : glob, true); });
    };
    invalid("Missing release asset list", json::object());
    invalid("Malformed release asset list", {{"assets", "invalid"}});
    invalid("No matching GPU release asset", {{"assets", json::array()}});
    auto ambiguous = release;
    ambiguous["assets"].push_back({{"name", "strata-engine-9.8.8-windows-x86_64-cuda-cuda12.tar.gz"}, {"digest", digest}});
    invalid("Ambiguous engine versions", ambiguous);
    for (const auto& bad_digest : {json(nullptr), json(123), json("sha1:" + std::string(40, 'a')),
                                  json("sha256:abc"), json("sha256:" + std::string(64, 'z'))}) {
        auto bad = published;
        bad["digest"] = bad_digest;
        invalid("Missing or invalid SHA-256 never permits an unverified download",
            {{"assets", json::array({bad})}});
    }
    auto missing_digest = published;
    missing_digest.erase("digest");
    invalid("Absent SHA-256 is explicit", {{"assets", json::array({missing_digest})}});
    invalid("Unsafe release asset filename", {{"assets", json::array({{{"name", "../engine.tar.gz"}, {"digest", digest}}})}}, "*");
    invalid("Release asset pattern is a glob, not a regex", release, "strata-engine-.*.tar.gz");
    const json unverified = {{"assets", json::array({{{"name", "engine-1.tar.gz"}}, {{"name", "engine-2.tar.gz"}}})}};
    check("Existing backends retain first-match discovery without mandatory digests",
        BackendUtils::select_release_asset(unverified, "engine-*.tar.gz").name == "engine-1.tar.gz");
}

void test_cache_isolation() {
    const auto write_file = [](const fs::path& path, const std::string& contents = "GGUF") {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << contents;
    };
    const auto repo = utils::path_from_utf8(utils::get_hf_cache_dir()) / "models--org--strata-shared";
    write_file(repo / "strata" / "UD-Q4_K_XL" / "model.gguf");
    write_file(repo / "strata" / "vision" / "main" / "mmproj.gguf");
    const auto user_models = utils::path_from_utf8(utils::get_config_dir()) / "user_models.json";
    if (fs::exists(user_models)) throw std::runtime_error("Cache isolation fixture requires an empty user registry");
    write_file(user_models, json({{"hf-strata-alias", {
        {"checkpoint", "org/strata-shared:model.gguf"}, {"mmproj", "mmproj.gguf"}, {"recipe", "llamacpp"}
    }}}).dump());
    ModelManager manager;
    const auto resolve_main = [&] {
        return llamacpp::resolve_gguf_path(utils::path_to_utf8(repo), "model.gguf");
    };
    CheckpointResolveContext context;
    context.hf_cache = utils::path_to_utf8(repo.parent_path());
    context.repo_id = "org/strata-shared";
    context.main_repo_id = context.repo_id;
    context.model_cache_path = utils::path_to_utf8(repo);
    context.variant = "mmproj.gguf";
    context.type = "mmproj";
    const auto resolve_aux = [&] {
        return default_backend_ops()->resolve_checkpoint_path({}, context);
    };
    check("Strata-only cache does not mark its llama.cpp alias downloaded",
        !manager.get_model_info("user.hf-strata-alias").downloaded);
    check("Strata GGUF is excluded from whole-repository checkpoint lookup", resolve_main().empty());
    check("Strata projector is excluded from auxiliary checkpoint lookup", resolve_aux().empty());
    context.variant.clear();
    check("Strata-only cache root is not a generic directory checkpoint", resolve_aux().empty());
    context.variant = "mmproj.gguf";
    context.repo_id = "org/projector";
    context.model_cache_path = utils::path_to_utf8(repo.parent_path() / "models--org--projector");
    check("Legacy auxiliary fallback does not claim another backend's private files", resolve_aux().empty());
    context.repo_id = context.main_repo_id;
    context.model_cache_path = utils::path_to_utf8(repo);
    write_file(repo / "README.md", "Repository metadata");
    check("Repository metadata beside Strata files does not imply a downloaded GGUF", resolve_main().empty());
    const auto snapshot = repo / "snapshots" / "registered";
    write_file(snapshot / "model.gguf");
    write_file(snapshot / "mmproj.gguf");
    write_file(repo / "refs" / "main", "registered");
    manager.invalidate_models_cache();
    check("Independent llama.cpp download still marks its entry downloaded",
        manager.get_model_info("user.hf-strata-alias").downloaded);
    check("Active registry GGUF is preferred over Strata's copy",
        resolve_main() == utils::path_to_utf8(snapshot / "model.gguf"));
    check("Active registry projector is preferred over Strata's copy",
        resolve_aux() == utils::path_to_utf8(snapshot / "mmproj.gguf"));
    write_file(snapshot / "strata" / "other.gguf");
    check("A genuine repository folder named strata is not backend-private",
        !hf_cache::is_backend_private_path(repo, snapshot / "strata" / "other.gguf"));
    check("Registry GGUFs nested under a genuine strata folder remain resolvable",
        llamacpp::resolve_gguf_path(utils::path_to_utf8(repo), "other.gguf") ==
            utils::path_to_utf8(snapshot / "strata" / "other.gguf"));
    const auto legacy = repo.parent_path() / "models--org--legacy-with-strata";
    write_file(legacy / "strata" / "private.gguf");
    write_file(legacy / "model.gguf");
    check("Flat legacy registry models remain resolvable beside private caches",
        llamacpp::resolve_gguf_path(utils::path_to_utf8(legacy), "model.gguf") ==
            utils::path_to_utf8(legacy / "model.gguf"));
    check("Private model files are preserved",
        fs::is_regular_file(repo / "strata" / "UD-Q4_K_XL" / "model.gguf") &&
        fs::is_regular_file(repo / "strata" / "vision" / "main" / "mmproj.gguf"));
    fs::remove(user_models);
}

void test_metadata(const fs::path& root) {
    const auto metadata = utils::JsonUtils::load_from_file(utils::get_resource_path("resources/strata.json"));
    const auto model_metadata = metadata.at("models").dump();
    check("Runtime model metadata has no fixed revisions, hashes or sizes",
        model_metadata.find("\"revision\"") == std::string::npos &&
        model_metadata.find("\"sha256\"") == std::string::npos &&
        model_metadata.find("\"size\"") == std::string::npos &&
        model_metadata.find("StrataCI") == std::string::npos);
    auto next = metadata.at("engine");
    next["repository"] = "maintainer/new-engine";
    next["asset_pattern"] = "engine_{variant}_*_{platform}.zip";
    next["targets"]["linux-x86_64"]["rocm"]["gfx9999"] = "future-gpu";
    next["default_context"] = 16384;
    next["arguments"] = {"--spec", "8"};
    const auto release = strata::parse_engine_release(next);
    check("Asset naming changes remain metadata-only", release.repository == "maintainer/new-engine" &&
        release.asset("linux", "x86_64", "rocm", "gfx9999") == "engine_future-gpu_*_linux-x86_64.zip" &&
        release.default_context == 16384 && release.arguments == std::vector<std::string>({"--spec", "8"}));
    next["arguments"] = {"--max-context=1"};
    rejects("Release tuning cannot override managed arguments", [&] { strata::parse_engine_release(next); });
    next["arguments"] = {"--spec", "8"};
    next["default_context"] = 12.5;
    rejects("Reject fractional default contexts", [&] { strata::parse_engine_release(next); });
    next["default_context"] = uint64_t{1} << 40;
    rejects("Reject oversized default contexts", [&] { strata::parse_engine_release(next); });
    next["default_context"] = 16384;
    next["protocol"] = 2;
    rejects("Incompatible pipe protocol is rejected", [&] { strata::parse_engine_release(next); });

    const auto registry = utils::JsonUtils::load_from_file(utils::get_resource_path("resources/server_models.json"));
    std::set<std::string> selections;
    for (const auto& [name, entry] : registry.items()) {
        if (entry.value("recipe", "") == "strata") {
            selections.insert(entry.at("checkpoint").get<std::string>());
            check("Curated checkpoint has download metadata",
                  !strata::model_checkpoint(entry.at("checkpoint").get<std::string>()).files.empty());
        }
    }
    const std::map<std::string, size_t> families = {
        {"ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF", 4},
        {"ukisai/Swift-1.5-Qwen3.8-Flash-Next-GSQ-RCO-GGUF", 3},
        {"ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-Coder-GGUF", 1},
        {"unsloth/Qwen3.8-Flash-Next-GGUF", 5},
        {"agentionai/Qwen3.8-Flash-Next-Gyro-GGUF", 2},
        {"agentionai/Qwen3.8-Flash-Next-Gyro-Coder-GGUF", 1},
        {"Maximilian228/Qwen3.8-Flash-Next-NVFP4-GPTQ-Strata", 1},
        {"Maximilian228/Qwen3.8-Flash-Next-NVIDIA-NVFP4-Strata", 1},
        {"unsloth/Qwen3.6-35B-A3B-MTP-GGUF", 3},
    };
    check("All 21 curated selections are public", selections.size() == 21);
    for (const auto& [repository, expected] : families) {
        const auto& quants = metadata.at("models").at(repository).at("quants");
        check("Every native family has all supported quants", quants.size() == expected);
        for (const auto& [quant, files] : quants.items()) {
            check("Every preparation selection has a registry model", selections.count(repository + ":" + quant) == 1);
        }
    }
    const auto native = strata::model_checkpoint("unsloth/Qwen3.6-35B-A3B-MTP-GGUF:Q8_0");
    const auto engine = strata::engine_release();
    const auto native_args = native.launch_arguments(engine, "rocm", root, root / "engine-data", 16384);
    check("Direct GGUF uses its own launch path and embedded MTP", native.preparation == "native-gguf" &&
        native.profile_file.empty() && std::find(native_args.begin(), native_args.end(), "--mtp") != native_args.end() &&
        std::find(native_args.begin(), native_args.end(), "--pack") == native_args.end() &&
        std::find(native_args.begin(), native_args.end(), "--spec") == native_args.end());
    check("Qwen3.6 does not download the Flash-Next drafter", !native.mtp.has_value());
    const auto flash = strata::model_checkpoint("ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF:IQ2_XS");
    const auto swift = strata::model_checkpoint("ukisai/Swift-1.5-Qwen3.8-Flash-Next-GSQ-RCO-GGUF:IQ2_XS");
    const auto gyro_coder = strata::model_checkpoint("agentionai/Qwen3.8-Flash-Next-Gyro-Coder-GGUF:Gyro-Coder");
    check("Gyro-Coder uses the default revision without an unrelated expert profile",
        gyro_coder.revision == "main" &&
        gyro_coder.files == std::vector<std::string>{"Qwen3.8-Flash-Next-Gyro-Coder-TQ2_0.gguf"} &&
        gyro_coder.compat_bf16 && gyro_coder.profile_file.empty() &&
        gyro_coder.mtp && gyro_coder.mtp->directory() == flash.mtp->directory() &&
        gyro_coder.vision && gyro_coder.vision->path() == flash.vision->path());
    rejects("Gyro-Coder-L stays unavailable until its new tensor formats are supported", [&] {
        strata::model_checkpoint("agentionai/Qwen3.8-Flash-Next-Gyro-Coder-GGUF:Gyro-Coder-L");
    });
    check("Flash-Next families share one MTP cache",
        flash.mtp && swift.mtp && flash.mtp->directory() == swift.mtp->directory());
    check("Vision defaults to CPU and has explicit placement/off controls",
        RecipeOptions("strata", {}).get_option("strata_vision") == "cpu" &&
        RecipeOptions("strata", {{"strata_vision", "gpu"}}).get_option("strata_vision") == "gpu" &&
        RecipeOptions("strata", {{"strata_vision", "off"}}).get_option("strata_vision") == "off");
    check("Vision token limits and GPU safety reserve follow upstream",
        engine.vision_cpu_max_tokens == 300 && engine.vision_gpu_max_tokens == 1024 && engine.vision_gpu_reserve_mib == 2048);
    for (const auto& selection : selections) {
        const auto checkpoint = strata::model_checkpoint(selection);
        check("Every curated Strata model has a default-revision projector",
            checkpoint.vision && checkpoint.vision->revision == "main" && !checkpoint.vision->file.empty());
    }
    check("Swift uses its fine-tuned vision projector",
        swift.vision && flash.vision && swift.vision->repository != flash.vision->repository &&
        swift.vision->file == "mmproj-Swift-Qwen3.8-Flash-Next-BF16.gguf");
    const auto coder = strata::model_checkpoint("ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-Coder-GGUF:IQ1_M");
    check("Identical original/Coder projectors share their download cache",
        flash.vision && coder.vision && flash.vision->path() == coder.vision->path());
    check("Qwen3.6 uses its own compatible projector",
        native.vision && native.vision->file == "mmproj-F16.gguf" &&
        native.vision->repository != flash.vision->repository);
    auto projector = swift.vision->identity();
    projector["file"] = "../outside.gguf";
    rejects("Projector paths cannot escape the cache", [&] { strata::parse_vision_pack(projector); });
    projector = swift.vision->identity();
    check("Projectors do not require duplicated file sizes or checksums",
        strata::parse_vision_pack(projector).file == swift.vision->file);
    auto vision_fixture = *swift.vision;
    vision_fixture.repository = "fixture/vision";
    vision_fixture.revision = "receipt-test";
    fs::create_directories(vision_fixture.directory());
    check("Projector is missing before a verified download receipt", !vision_fixture.present());
    std::ofstream(vision_fixture.path(), std::ios::binary) << "fixture";
    auto legacy_vision_identity = vision_fixture.identity();
    legacy_vision_identity["release"] = "previous-release";
    strata::record_downloads(vision_fixture.directory(), "lemonade-vision.json", legacy_vision_identity,
        {vision_fixture.file});
    check("Verified projector receipts support offline reuse", vision_fixture.present());
    const auto vision_directory = vision_fixture.directory().parent_path() / "legacy-release";
    fs::rename(vision_fixture.directory(), vision_directory);
    check("Engine upgrades reuse legacy release-scoped projector files in place",
        vision_fixture.directory() == vision_directory && vision_fixture.present());
    fs::resize_file(vision_fixture.path(), 1);
    check("Truncated projector is not ready", !vision_fixture.present());
    fs::resize_file(vision_fixture.path(), 0);
    rejects("Empty projector cannot be marked downloaded", [&] {
        strata::record_downloads(vision_fixture.directory(), "lemonade-vision.json", vision_fixture.identity(),
            {vision_fixture.file});
    });
    const strata::MemoryCapacity ample{64, 16};
    const auto gyro_coder_args = gyro_coder.launch_arguments(engine, "rocm", root, root / "engine-data", 8192);
    check("Profile-free Gyro-Coder uses the engine's native cache admission",
        std::find(gyro_coder_args.begin(), gyro_coder_args.end(), "--expert-profile") == gyro_coder_args.end() &&
        std::find(gyro_coder_args.begin(), gyro_coder_args.end(), "--expert-cache") != gyro_coder_args.end() &&
        gyro_coder.memory_plan(engine, "rocm", 8192, "auto", "auto", "auto", ample, false).experts == "mmap");
    rejects("Profile-free models cannot request static-profile resident experts", [&] {
        gyro_coder.memory_plan(engine, "rocm", 8192, "auto", "resident", "auto", ample, false);
    });
    const auto plan = [&](const strata::MemoryCapacity& capacity, int ctx = 4096,
                          const std::string& kv = "auto", const std::string& experts = "auto",
                          const std::string& streaming = "auto", bool custom = false) {
        return swift.memory_plan(engine, "rocm", ctx, kv, experts, streaming, capacity, custom);
    };
    check("Full-arena qualification comes from checkpoint metadata", swift.full_expert_arena);
    check("Native arena does not depend on available-memory estimates", plan({}).experts == "arena");
    check("Unknown capacity disables automatic streaming", plan({}, 262144).kv_resident == 0);
    const auto parity = plan({}, 262144, "int8", "auto", "on");
    auto parity_args = swift.launch_arguments(engine, "rocm", root, root / "engine-data", 262144, true, "int8");
    parity.apply(parity_args);
    check("Explicit full-arena and streamed-KV combination remains available",
        parity.experts == "arena" && parity.kv_resident == 32768 &&
        std::find(parity_args.begin(), parity_args.end(), "--mmap-experts") == parity_args.end() &&
        std::find(parity_args.begin(), parity_args.end(), "--resident-experts") == parity_args.end() &&
        std::find(parity_args.begin(), parity_args.end(), "--kv-resident") != parity_args.end());
    auto official_budget = ample;
    official_budget.total_ram_gib = 63.4;
    official_budget.total_vram_gib = 15.92;
    const auto automatic_parity = plan(official_budget, 262144, "auto", "auto");
    auto automatic_args = swift.launch_arguments(engine, "rocm", root, root / "engine-data", 262144);
    automatic_parity.apply(automatic_args);
    const auto arg_value = [&](const std::string& flag) {
        const auto at = std::find(automatic_args.begin(), automatic_args.end(), flag);
        return at != automatic_args.end() && std::next(at) != automatic_args.end() ? *std::next(at) : "";
    };
    check("Default Swift launch matches official arena, KV, reserve and speculation settings without overrides",
        automatic_parity.experts == "arena" && automatic_parity.kv_resident == 32768 &&
        automatic_parity.vram_reserve_mib == 1536 &&
        arg_value("--kv") == "int8" && arg_value("--kv-resident") == "32768" &&
        arg_value("--vram-reserve-mib") == "1536" && arg_value("--spec") == "4" &&
        arg_value("--spec-min-p") == "0.5" &&
        std::find(automatic_args.begin(), automatic_args.end(), "--mmap-experts") == automatic_args.end());
    const double native_ram_boundary = (48 + 262144.0 * 13 * 1056 / 1e9 + 1) / 1.073741824;
    official_budget.total_ram_gib = native_ram_boundary + 0.001;
    check("Native streaming follows the upstream RAM-capacity threshold above the boundary",
        plan(official_budget, 262144, "auto", "auto").kv_resident == 32768);
    official_budget.total_ram_gib -= 0.002;
    check("Native streaming stays off below its RAM-capacity boundary",
        plan(official_budget, 262144, "auto", "auto").kv_resident == 0);
    official_budget.total_ram_gib = 63.4;
    check("Native streaming respects an explicit off",
        plan(official_budget, 262144, "auto", "auto", "off").kv_resident == 0);
    for (const auto& [capacity, reserve] : std::vector<std::pair<double, int>>{{8, 1536}, {16, 1536}, {24, 4915}}) {
        official_budget.total_vram_gib = capacity;
        check("App work-profile VRAM reserve follows card capacity",
            plan(official_budget).vram_reserve_mib == reserve);
    }
    official_budget.unified_gpu = true;
    check("Unified GPUs retain the display floor without reserving a fifth of RAM",
        plan(official_budget).vram_reserve_mib == 1024);
    for (const auto& override_args : std::vector<std::vector<std::string>>{
            {"--vram-reserve-mib", "2560"}, {"--vram-reserve-mib=2560"}}) {
        auto args = swift.launch_arguments(engine, "rocm", root, root / "engine-data", 4096);
        args.insert(args.end(), override_args.begin(), override_args.end());
        automatic_parity.apply(args);
        check("Native reserve recommendation preserves a user override",
            std::count(args.begin(), args.end(), "--vram-reserve-mib") +
            std::count(args.begin(), args.end(), "--vram-reserve-mib=2560") == 1 &&
            (std::find(args.begin(), args.end(), "2560") != args.end() ||
             std::find(args.begin(), args.end(), "--vram-reserve-mib=2560") != args.end()));
    }
    const auto mapped_stream = plan(ample, 262144, "auto", "mmap");
    check("Explicit expert modes share the native KV capacity recommendation",
        mapped_stream.experts == "mmap" && mapped_stream.kv_resident == 32768 &&
        plan(ample, 262144, "auto", "resident").kv_resident == 32768);
    check("Mapped streaming preserves explicit off",
        plan(ample, 262144, "auto", "mmap", "off").kv_resident == 0);
    auto mapped_stream_args = swift.launch_arguments(engine, "rocm", root, root / "engine-data", 262144);
    mapped_stream.apply(mapped_stream_args);
    const auto streamed_window = std::find(mapped_stream_args.begin(), mapped_stream_args.end(), "--kv-resident");
    check("Mapped KV launch keeps mmap and adds the release's resident window",
        std::find(mapped_stream_args.begin(), mapped_stream_args.end(), "--mmap-experts") != mapped_stream_args.end() &&
        std::find(mapped_stream_args.begin(), mapped_stream_args.end(), "--resident-experts") == mapped_stream_args.end() &&
        streamed_window != mapped_stream_args.end() && std::next(streamed_window) != mapped_stream_args.end() &&
        *std::next(streamed_window) == "32768");
    auto capacity = ample;
    for (const double unknown : {-1.0, std::numeric_limits<double>::infinity(),
                                  std::numeric_limits<double>::quiet_NaN()}) {
        capacity = ample;
        capacity.total_ram_gib = unknown;
        check("Unknown/nonfinite RAM disables automatic streaming", plan(capacity, 262144).kv_resident == 0);
        capacity = ample;
        capacity.total_vram_gib = unknown;
        check("Unknown/nonfinite VRAM retains the engine reserve", plan(capacity).vram_reserve_mib == -1);
    }
    capacity = ample;
    capacity.unified_gpu = true;
    check("Unified memory disables automatic KV streaming", plan(capacity, 262144).kv_resident == 0);
    check("Unified memory defaults to direct PLE reads even with unrelated custom tuning",
        plan(capacity).ple_direct && plan(capacity, 4096, "auto", "auto", "auto", true).ple_direct);
    auto ple_args = swift.launch_arguments(engine, "rocm", root, root / "engine-data", 4096);
    plan(capacity).apply(ple_args);
    const auto ple_flag = std::find(ple_args.begin(), ple_args.end(), "--ple-io");
    check("Unified memory injects direct PLE reads", ple_flag != ple_args.end() && *std::next(ple_flag) == "direct");
    for (const auto& mode : {"ram", "mmap", "direct"}) {
        for (const bool equals : {false, true}) {
            auto args = swift.launch_arguments(engine, "rocm", root, root / "engine-data", 4096);
            if (equals) args.push_back(std::string("--ple-io=") + mode);
            else args.insert(args.end(), {"--ple-io", mode});
            const auto original = args;
            plan(capacity).apply(args);
            check("Explicit PLE mode is preserved", std::count_if(args.begin(), args.end(), [](const auto& arg) {
                return arg == "--ple-io" || arg.rfind("--ple-io=", 0) == 0;
            }) == 1 && std::search(args.begin(), args.end(), original.end() - (equals ? 1 : 2), original.end()) != args.end());
        }
    }
    check("Discrete GPUs retain engine PLE policy", !plan(ample).ple_direct);
    check("Metal and direct-GGUF models retain native PLE behavior",
        !swift.memory_plan(engine, "metal", 4096, "auto", "auto", "auto", capacity, false).ple_direct &&
        !native.memory_plan(engine, "rocm", 4096, "auto", "auto", "auto", capacity, false).ple_direct);
    capacity = ample;
    capacity.wsl = true;
    check("WSL never enables automatic KV streaming", plan(capacity, 262144).kv_resident == 0);
    rejects("WSL rejects forced KV streaming", [&] { plan(capacity, 262144, "auto", "auto", "on"); });
    check("Custom tuning disables automatic KV streaming without changing expert mode",
        plan(ample, 262144, "auto", "auto", "auto", true).experts == "arena" &&
        plan(ample, 262144, "auto", "auto", "auto", true).kv_resident == 0);
    check("Explicit modes are preserved even without measurements",
        plan({}, 4096, "auto", "resident").experts == "resident" &&
        plan(ample, 4096, "auto", "mmap").experts == "mmap" &&
        plan({}, 262144, "auto", "mmap", "on").kv_resident == 32768);
    check("Automatic KV streaming follows the Strata threshold and window",
        plan(ample, 65535).kv_resident == 0 && plan(ample, 65536).kv_resident == 32768 &&
        plan(ample, 262144).kv_resident == 32768);
    check("Explicit KV streaming off is preserved", plan(ample, 262144, "auto", "auto", "off").kv_resident == 0);
    rejects("Streaming below its minimum context is explicit", [&] {
        plan(ample, 32768, "auto", "auto", "on");
    });
    capacity = ample;
    capacity.total_ram_gib = 50;
    check("KV precision changes the RAM streaming fit",
        plan(capacity, 262144, "fp16").kv_resident == 0 &&
        plan(capacity, 262144, "q4_0").kv_resident == 32768);
    auto unknown_arena = swift;
    unknown_arena.full_expert_arena = false;
    unknown_arena.recommended_ram_gb = 0;
    check("Runtime recommendation upgrades do not invalidate prepared model weights",
        unknown_arena.identity() == swift.identity());
    check("Unqualified models stay mapped",
        unknown_arena.memory_plan(engine, "rocm", 262144, "auto", "auto", "auto", ample, false).experts == "mmap" &&
        unknown_arena.memory_plan(engine, "rocm", 262144, "auto", "auto", "auto", ample, false).kv_resident == 0);
    check("Metal and Qwen3.6 preserve native expert behavior",
        swift.memory_plan(engine, "metal", 4096, "auto", "auto", "auto", ample, false).experts.empty() &&
        native.memory_plan(engine, "rocm", 4096, "auto", "auto", "auto", ample, false).experts.empty());
    for (const auto& unsupported : {"invalid", "RESIDENT", "guarded"}) {
        rejects("Invalid expert mode is rejected", [&] {
            plan(ample, 4096, "auto", unsupported);
        });
    }
    rejects("Invalid streaming mode is rejected", [&] { plan(ample, 262144, "auto", "auto", "invalid"); });
    rejects("Metal rejects explicit expert mode", [&] {
        swift.memory_plan(engine, "metal", 4096, "auto", "resident", "auto", ample, false);
    });
    rejects("Qwen3.6 rejects explicit expert mode", [&] {
        native.memory_plan(engine, "rocm", 4096, "auto", "resident", "auto", ample, false);
    });
    rejects("Metal rejects explicit KV streaming", [&] {
        swift.memory_plan(engine, "metal", 262144, "auto", "auto", "on", ample, false);
    });
    auto cell_release = metadata.at("engine");
    cell_release["kv_cache"]["cell_bytes"]["fp16"] = 1.5;
    rejects("KV cell sizes must be integral", [&] { strata::parse_engine_release(cell_release); });
    auto bad_ram = metadata.at("models");
    bad_ram[swift.repository]["quant_options"][swift.quant]["recommended_ram_gb"] = -1;
    rejects("Recommended RAM must be positive", [&] {
        strata::parse_model_checkpoint(swift.repository + ":" + swift.quant, bad_ram);
    });
    check("Expert mode defaults to automatic", RecipeOptions("strata", {}).get_option("strata_experts") == "auto");
    check("KV streaming defaults to automatic", RecipeOptions("strata", {}).get_option("strata_kv_streaming") == "auto");
    for (const auto& backend : {"cuda", "cuda13", "rocm"}) {
        auto args = swift.launch_arguments(engine, backend, root, root / "engine-data", 262144);
        plan(ample, 262144).apply(args);
        check("Arena launch does not force mmap or the partial resident mode",
            std::find(args.begin(), args.end(), "--mmap-experts") == args.end() &&
            std::find(args.begin(), args.end(), "--resident-experts") == args.end());
        const auto window = std::find(args.begin(), args.end(), "--kv-resident");
        check("Native streaming launch uses the recommended VRAM window",
            window != args.end() && std::next(window) != args.end() && *std::next(window) == "32768");
    }
    auto resident_args = swift.launch_arguments(engine, "rocm", root, root / "engine-data", 4096);
    plan({}, 4096, "auto", "resident").apply(resident_args);
    check("Manual resident mode remains available",
        std::find(resident_args.begin(), resident_args.end(), "--resident-experts") != resident_args.end() &&
        std::find(resident_args.begin(), resident_args.end(), "--mmap-experts") == resident_args.end());
    auto budget_model = strata::parse_model_checkpoint(
        "unsloth/Qwen3.8-Flash-Next-GGUF:UD-Q4_K_XL", metadata.at("models"));
    auto tuned_capacity = ample;
    tuned_capacity.expert_size_gib = 70;
    tuned_capacity.performance_cores = 8;
    tuned_capacity.efficiency_cores = 16;
    const auto budget_plan = [&](const strata::MemoryCapacity& capacity, int ctx = 4096,
                                 const std::string& experts = "auto", const std::string& streaming = "off",
                                 bool custom = false) {
        return budget_model.memory_plan(engine, "rocm", ctx, "int8", experts, streaming, capacity, custom);
    };
    const auto budget = budget_plan(tuned_capacity);
    check("Unsloth models qualify for a measured RAM budget",
        budget_model.budget_experts && budget.experts == "budget" && budget.resident_budget_gib == 40);
    check("Hybrid CPU recommendation uses physical P cores and half E cores", budget.pool_workers == 15);
    auto direct_model = budget_model;
    direct_model.preparation = "native-gguf";
    const auto direct_plan = direct_model.memory_plan(engine, "rocm", 4096,
        "auto", "auto", "auto", tuned_capacity, false);
    std::vector<std::string> direct_args;
    direct_plan.apply(direct_args);
    check("Direct GGUF also inherits CPU recommendations without packed memory options",
        direct_args == std::vector<std::string>({"--pool-workers", "15"}));
    auto budget_args = budget_model.launch_arguments(engine, "rocm", root, root / "engine-data", 4096);
    budget.apply(budget_args);
    const auto budget_flag = std::find(budget_args.begin(), budget_args.end(), "--resident-budget-gib");
    check("Budget launch removes full-arena selection and passes the exact GiB budget",
        budget_flag != budget_args.end() && *std::next(budget_flag) == "40" &&
        std::find(budget_args.begin(), budget_args.end(), "--mmap-experts") == budget_args.end());
    const auto streamed_budget = budget_plan(tuned_capacity, 262144, "auto", "on");
    const double kv_gib = 262144.0 * engine.kv_layers * engine.kv_cell_bytes.at("int8") / (1024.0 * 1024 * 1024);
    check("Streamed KV RAM is deducted before choosing the expert budget",
        streamed_budget.resident_budget_gib == 40 - static_cast<int>(std::ceil(kv_gib)));
    check("Explicit expert modes bypass automatic budgeting",
        budget_plan(tuned_capacity, 4096, "mmap").experts == "mmap" &&
        budget_plan(tuned_capacity, 4096, "resident").experts == "resident");
    check("Custom tuning bypasses automatic budget and CPU recommendations",
        budget_plan(tuned_capacity, 4096, "auto", "off", true).resident_budget_gib == 0 &&
        budget_plan(tuned_capacity, 4096, "auto", "off", true).pool_workers == 0);
    auto limited = tuned_capacity;
    limited.total_ram_gib = 31.9;
    check("Insufficient RAM keeps mmap rather than forcing the minimum budget", budget_plan(limited).experts == "mmap");
    limited.total_ram_gib = 32;
    check("Minimum eight-GiB budget is inclusive", budget_plan(limited).resident_budget_gib == 8);
    limited.total_ram_gib = 86;
    check("Full arena uses measured expert size plus headroom", budget_plan(limited).experts == "arena");
    limited.total_ram_gib = 85.9;
    check("Below full-arena threshold retains the bounded mode", budget_plan(limited).experts == "budget");
    limited.total_ram_gib = -1;
    check("Unknown RAM retains engine memory defaults", budget_plan(limited).experts == "mmap");
    limited = tuned_capacity;
    limited.expert_size_gib = 0;
    check("Unknown expert size does not invent a budget", budget_plan(limited).experts == "mmap");
    limited = tuned_capacity;
    limited.unified_gpu = true;
    check("Shared GPU RAM is not double-budgeted", budget_plan(limited).experts == "mmap");
    limited = tuned_capacity;
    limited.efficiency_cores = 8;
    check("Balanced hybrid CPUs retain engine worker defaults", budget_plan(limited).pool_workers == 0);
    limited.performance_cores = 0;
    limited.efficiency_cores = 64;
    check("Unknown performance-core count retains defaults", budget_plan(limited).pool_workers == 0);
    limited.performance_cores = 16;
    check("Hybrid recommendation obeys the release worker cap", budget_plan(limited).pool_workers == 32);
    for (const auto& backend : {"cuda", "cuda13", "rocm"}) {
        for (const auto& experts : {"auto", "mmap", "resident"}) {
            auto args = swift.launch_arguments(engine, backend, root, root / "engine-data", 262144);
            plan(ample, 262144, "auto", experts).apply(args);
            check("PLE residency remains engine-managed for every packed expert mode",
                std::find(args.begin(), args.end(), "--ple-io") == args.end());
        }
    }
    check("Budgeted experts leave PLE residency to the engine",
        std::find(budget_args.begin(), budget_args.end(), "--ple-io") == budget_args.end());
    for (const auto& raw : {"--pool-workers", "--pool-workers=7", "--cpu-threads", "--cpu-threads=7"}) {
        std::vector<std::string> args{raw};
        if (std::string(raw).find('=') == std::string::npos) args.push_back("7");
        const auto original = args;
        strata::MemoryPlan worker_plan;
        worker_plan.pool_workers = 15;
        worker_plan.apply(args);
        check("Explicit worker flag remains untouched", args == original);
    }
    for (const auto& mode : {"ram", "direct", "mmap"}) {
        auto args = swift.launch_arguments(engine, "rocm", root, root / "engine-data", 4096);
        args.insert(args.end(), {"--ple-io", mode});
        plan(ample, 4096, "auto", "auto", "auto", true).apply(args);
        const auto ple = std::find(args.begin(), args.end(), "--ple-io");
        check("Explicit PLE mode is preserved without an automatic override",
            std::count(args.begin(), args.end(), "--ple-io") == 1 &&
            ple != args.end() && std::next(ple) != args.end() && *std::next(ple) == mode);
    }
    auto tuned_release = metadata.at("engine");
    tuned_release["memory_tuning"]["expert_ram_headroom_gib"] = 28;
    tuned_release["memory_tuning"]["pool_workers_cap"] = 12;
    const auto changed_tuning = budget_model.memory_plan(strata::parse_engine_release(tuned_release),
        "rocm", 4096, "int8", "auto", "off", tuned_capacity, false);
    check("Budget and worker tuning comes from release metadata",
        changed_tuning.resident_budget_gib == 36 && changed_tuning.pool_workers == 12);
    tuned_release["memory_tuning"]["expert_min_budget_gib"] = 0;
    rejects("RAM tuning thresholds must be positive", [&] { strata::parse_engine_release(tuned_release); });
    for (const auto& raw : {"--kv-resident", "--kv-resident=20480"}) {
        auto args = swift.launch_arguments(engine, "rocm", root, root / "engine-data", 262144);
        args.push_back(raw);
        rejects("Conflicting raw KV streaming flags are rejected", [&] { plan(ample, 262144).apply(args); });
    }
    auto streaming_release = metadata.at("engine");
    streaming_release["kv_cache"]["streaming_context"] = 131072;
    streaming_release["kv_cache"]["resident_context"] = 65536;
    check("Streaming recommendations are release metadata",
        swift.memory_plan(strata::parse_engine_release(streaming_release), "rocm", 131072,
            "auto", "auto", "auto", ample, false).kv_resident == 65536);
    streaming_release["kv_cache"]["resident_context"] = 131072;
    rejects("Streaming window must be smaller than its threshold", [&] { strata::parse_engine_release(streaming_release); });
    streaming_release["kv_cache"]["resident_context"] = 1.5;
    rejects("Streaming context must be integral", [&] { strata::parse_engine_release(streaming_release); });
    check("Raw expert modes cannot bypass managed options",
        !utils::validate_custom_args("--resident-experts", strata::managed_arguments()).empty() &&
        !utils::validate_custom_args("--mmap-experts", strata::managed_arguments()).empty());
    for (const auto& backend : {"cuda", "cuda13", "rocm"}) {
        const auto args = swift.launch_arguments(engine, backend, root, root / "engine-data", 4096);
        const auto flag = std::find(args.begin(), args.end(), "--mtp");
        check("MTP is enabled on " + std::string(backend), flag != args.end() &&
            std::next(flag) != args.end() && *std::next(flag) == utils::path_to_utf8(swift.mtp->directory()) &&
            std::find(args.begin(), args.end(), "--spec-min-p") != args.end());
        const auto vocabulary = std::find(args.begin(), args.end(), "--mtp-draft-vocab");
        const auto profile = std::find(args.begin(), args.end(), "--expert-profile");
        check("MTP vocabulary and expert profile use the engine-owned data directory",
            vocabulary != args.end() && utils::path_from_utf8(*std::next(vocabulary)).lexically_normal() ==
                (root / "engine-data" / "data" / "draft_vocab.bin").lexically_normal() &&
            profile != args.end() && utils::path_from_utf8(*std::next(profile)).lexically_normal() ==
                (root / "engine-data" / swift.profile_file).lexically_normal());
        const auto disabled = swift.launch_arguments(engine, backend, root, root / "engine-data", 4096, false);
        check("MTP can be disabled on " + std::string(backend),
            std::find(disabled.begin(), disabled.end(), "--mtp") == disabled.end() &&
            std::find(disabled.begin(), disabled.end(), "--spec-min-p") == disabled.end() &&
            std::find(disabled.begin(), disabled.end(), "--spec") != disabled.end());
    }
    const auto metal_args = swift.launch_arguments(engine, "metal", root, root / "engine-data", 4096);
    check("Metal does not receive incompatible MTP runtime files",
        std::find(metal_args.begin(), metal_args.end(), "--mtp") == metal_args.end());
    const auto value_after = [](const std::vector<std::string>& args, const std::string& flag) {
        const auto found = std::find(args.begin(), args.end(), flag);
        return found == args.end() || std::next(found) == args.end() ? std::string() : *std::next(found);
    };
    for (const auto& selection : selections) {
        const auto checkpoint = strata::model_checkpoint(selection);
        for (const auto& backend : checkpoint.backends) {
            const auto enabled = checkpoint.launch_arguments(engine, backend, root, root / "engine-data", 262144, true);
            const auto disabled = checkpoint.launch_arguments(engine, backend, root, root / "engine-data", 262144, false);
            const bool embedded = checkpoint.embedded_mtp_drafts > 0;
            const bool external = checkpoint.mtp && checkpoint.mtp->backends.count(backend);
            check("Every model/backend selects only its compatible MTP: " + selection + "/" + backend,
                value_after(enabled, "--mtp") == (embedded ? "2" :
                    external ? utils::path_to_utf8(checkpoint.mtp->directory()) : "") &&
                value_after(disabled, "--mtp") == (embedded ? "0" : "") &&
                std::count(enabled.begin(), enabled.end(), "--mtp") == (embedded || external ? 1 : 0) &&
                std::count(disabled.begin(), disabled.end(), "--mtp") == (embedded ? 1 : 0));
            check("Every direct GGUF avoids pack-only flags",
                !embedded || (value_after(enabled, "--pack").empty() &&
                    value_after(enabled, "--kv").empty() && value_after(enabled, "--spec").empty() &&
                    value_after(enabled, "--expert-profile").empty()));
        }
    }
    native.validate_custom_arguments(
        "--prefill auto --chunk 512 --cpu-threads 8 --pool-workers 8 --expert-vram 8.5 "
        "--expert-slots 16 --hybrid-rows 128 --draft-vocab 65536 --gpu 0 --no-warmup "
        "--no-token-graph --no-graph");
    check("Qwen3.6 accepts supported native tuning flags", true);
    for (const auto& unsupported : {"--spec 4", "--spec-min-p 0.5", "--expert-cache auto",
             "--kv-resident 32768", "--vram-reserve-mib 2048", "--batch-mtp",
             "--prefill=512", "--unknown", "--prefill", "--prefill --no-graph"}) {
        rejects("Qwen3.6 rejects ignored or malformed tuning: " + std::string(unsupported),
            [&] { native.validate_custom_arguments(unsupported); });
    }
    for (const auto& checkpoint : {native, swift}) {
        for (const auto& managed : {"--mtp 2", "--mtp=0", "--ctx 4096", "--model other.gguf",
                 "--slots 2", "--emb other.sve", "--bench-pp 4096"}) {
            rejects("Managed aliases cannot bypass Strata options",
                [&] { checkpoint.validate_custom_arguments(managed); });
        }
    }
    auto embedded_models = metadata.at("models");
    auto& embedded_metadata = embedded_models.at(native.repository);
    for (const auto& count : {json(0), json(8), json(1.5)}) {
        embedded_metadata["embedded_mtp_drafts"] = count;
        rejects("Embedded draft count must be a supported integer", [&] {
            strata::parse_model_checkpoint(native.repository + ":Q8_0", embedded_models);
        });
    }
    embedded_metadata["embedded_mtp_drafts"] = 2;
    embedded_metadata["arguments"] = {"--mtp", "2"};
    rejects("Model tuning cannot bypass the MTP toggle", [&] {
        strata::parse_model_checkpoint(native.repository + ":Q8_0", embedded_models);
    });
    embedded_metadata["arguments"] = {"--prefill", "auto"};
    embedded_metadata["mtp"] = metadata.at("models").at(flash.repository).at("mtp");
    rejects("Embedded and external MTP cannot be mixed", [&] {
        strata::parse_model_checkpoint(native.repository + ":Q8_0", embedded_models);
    });
    ModelInfo native_info;
    native_info.recipe = "strata";
    native_info.checkpoints["main"] = native.repository + ":Q8_0";
    native_info.checkpoints["draft"] = "other/model:llama-draft.gguf";
    RecipeOptions native_options("strata", {{"strata_backend", "rocm"}});
    rejects("Llama.cpp draft checkpoints fail before Strata downloads or starts", [&] {
        strata::ops()->resolve_runtime_options(native_info, native_options);
    });
    native_info.checkpoints.erase("draft");
    strata::ops()->resolve_runtime_options(native_info, native_options);
    check("Embedded MTP needs no draft checkpoint role", true);
    native_options.set_option("strata_args", "--spec 4");
    rejects("Unsupported direct-GGUF flags fail before Strata downloads or starts", [&] {
        strata::ops()->resolve_runtime_options(native_info, native_options);
    });
    for (const auto& backend : {"cuda", "cuda13", "rocm"}) {
        for (const int context : {4096, 8192, 8193, 262144}) {
            check("KV auto follows Strata context recommendations",
                value_after(swift.launch_arguments(engine, backend, root, root / "engine-data", context), "--kv") ==
                    (context <= 8192 ? "fp16" : "int8"));
        }
        for (const auto& format : {"fp16", "int8", "q4_0", "k8v4"}) {
            check("Explicit KV precision is preserved",
                value_after(swift.launch_arguments(engine, backend, root, root / "engine-data", 4096, true, format), "--kv") == format &&
                value_after(swift.launch_arguments(engine, backend, root, root / "engine-data", 262144, true, format), "--kv") == format);
        }
    }
    check("Metal and Qwen3.6 keep their native cache defaults",
        value_after(metal_args, "--kv").empty() && value_after(native_args, "--kv").empty());
    rejects("Unknown KV cache format is rejected", [&] {
        swift.launch_arguments(engine, "rocm", root, root / "engine-data", 4096, true, "invalid");
    });
    rejects("Metal does not silently ignore explicit KV precision", [&] {
        swift.launch_arguments(engine, "metal", root, root / "engine-data", 4096, true, "int8");
    });
    rejects("Qwen3.6 does not receive unsupported KV arguments", [&] {
        native.launch_arguments(engine, "rocm", root, root / "engine-data", 4096, true, "int8");
    });
    auto kv_release = metadata.at("engine");
    kv_release["kv_cache"]["short_context"] = 16384;
    kv_release["kv_cache"]["long_format"] = "q4_0";
    const auto changed_kv = strata::parse_engine_release(kv_release);
    check("KV recommendations are release-metadata driven",
        value_after(swift.launch_arguments(changed_kv, "rocm", root, root / "engine-data", 16384), "--kv") == "fp16" &&
        value_after(swift.launch_arguments(changed_kv, "rocm", root, root / "engine-data", 16385), "--kv") == "q4_0");
    kv_release["kv_cache"]["short_context"] = 1.5;
    rejects("KV threshold must be an integer", [&] { strata::parse_engine_release(kv_release); });
    kv_release["kv_cache"]["short_context"] = 8192;
    kv_release["kv_cache"]["long_format"] = "invalid";
    rejects("KV recommendations must name supported formats", [&] { strata::parse_engine_release(kv_release); });
    check("MTP option defaults on and can be disabled",
        RecipeOptions("strata", {}).get_option("strata_mtp") == true &&
        RecipeOptions("strata", {{"strata_mtp", false}}).get_option("strata_mtp") == false);
    auto draft_metadata = metadata.at("models").at(flash.repository).at("mtp");
    const auto& vocabulary = flash.mtp->files.at("mtp/draft_vocab.bin");
    check("Default draft vocabulary comes from the official release",
        vocabulary.source == "data/draft_vocab.bin" &&
        engine.data_url(BackendUtils::get_backend_version("strata", "rocm"), vocabulary.source) ==
            "https://raw.githubusercontent.com/" + engine.repository + "/" +
            BackendUtils::get_backend_version("strata", "rocm") + "/data/draft_vocab.bin");
    check("Future releases require no data URL or checksum maintenance",
        engine.data_url("v9.8.7", flash.profile_file) ==
            "https://raw.githubusercontent.com/" + engine.repository + "/v9.8.7/data/expert-profile.bin");
    rejects("Release data paths reject traversal", [&] { engine.data_url("v9.8.7", "../outside"); });
    auto invalid_draft = draft_metadata;
    check("MTP uses the repository default revision", strata::parse_draft_pack(invalid_draft).revision == "main");
    invalid_draft["files"]["mtp/draft_vocab.bin"]["source"] = "../outside";
    rejects("Draft sources reject traversal", [&] { strata::parse_draft_pack(invalid_draft); });
    invalid_draft = draft_metadata;
    invalid_draft["files"]["../dense.bin"] = invalid_draft["files"]["mtp/dense.bin"];
    rejects("MTP rejects traversal", [&] { strata::parse_draft_pack(invalid_draft); });
    invalid_draft = draft_metadata;
    invalid_draft["files"]["other/DENSE.BIN"] = invalid_draft["files"]["mtp/dense.bin"];
    rejects("MTP rejects case-insensitive file collisions", [&] { strata::parse_draft_pack(invalid_draft); });
    invalid_draft = draft_metadata;
    invalid_draft["files"].erase("mtp/experts.bin");
    rejects("MTP requires all native runtime files", [&] { strata::parse_draft_pack(invalid_draft); });
    invalid_draft = draft_metadata;
    invalid_draft["arguments"] = {"--mtp", "outside"};
    rejects("MTP tuning cannot override managed files", [&] { strata::parse_draft_pack(invalid_draft); });
    auto draft = *flash.mtp;
    fs::create_directories(draft.directory());
    for (const auto& [name, file] : draft.files) {
        std::ofstream(draft.directory() / fs::path(name).filename()) << "abc";
    }
    check("Unverified MTP files are not ready", !draft.present());
    std::vector<fs::path> draft_paths;
    for (const auto& [name, file] : draft.files) draft_paths.push_back(fs::path(name).filename());
    auto legacy_draft_identity = draft.identity();
    legacy_draft_identity["release"] = "previous-release";
    strata::record_downloads(draft.directory(), "lemonade-draft.json", legacy_draft_identity, draft_paths);
    check("MTP receipt records actual downloaded sizes", draft.present());
    const auto draft_directory = draft.directory().parent_path() / "legacy-release";
    fs::rename(draft.directory(), draft_directory);
    check("Legacy MTP cache directories are reused in place", draft.directory() == draft_directory && draft.present());
    auto changed_draft = draft;
    changed_draft.files.at("mtp/draft_vocab.bin").source = "data/draft_vocab_en.bin";
    check("Draft source changes do not invalidate cached MTP weights", changed_draft.present());
    changed_draft = draft;
    check("Engine upgrades reuse MTP weights independently of engine-owned vocabulary",
        changed_draft.directory() == draft.directory() && changed_draft.present() &&
        changed_draft.file_present("mtp/dense.bin") && changed_draft.file_present("mtp/experts.bin") &&
        !changed_draft.file_present("mtp/draft_vocab.bin"));
    strata::record_downloads(changed_draft.directory(), "lemonade-draft.json", changed_draft.identity(), draft_paths);
    check("New draft receipts preserve the legacy weight directory",
        changed_draft.present() && changed_draft.directory() == draft_directory);
    fs::remove(draft.directory() / "draft_vocab.bin");
    check("Cached MTP weights do not require a per-model vocabulary copy", changed_draft.present());
    fs::resize_file(draft.directory() / "experts.bin", 1);
    check("Truncated MTP is not ready", !changed_draft.present());
    rejects("Qwen3.6 rejects Metal before launch", [&] { native.launch_arguments(engine, "metal", root, root / "engine-data", 0); });
    const auto ready = strata::model_checkpoint("Maximilian228/Qwen3.8-Flash-Next-NVFP4-GPTQ-Strata:NVFP4");
    const auto ready_args = ready.launch_arguments(engine, "cuda", root, root / "engine-data", 0);
    check("NVFP4 launches with separate dense, PLE and embedding inputs",
        std::find(ready_args.begin(), ready_args.end(), "--native-dense-gguf") != ready_args.end() &&
        std::find(ready_args.begin(), ready_args.end(), "--ple-gguf") != ready_args.end() &&
        std::find(ready_args.begin(), ready_args.end(), "--embd-gguf") != ready_args.end() &&
        ready.local_file(root, "pack/dense.bin") == root / "pack" / "dense.bin");
    rejects("NVFP4 refuses HIP before download or launch", [&] { ready.launch_arguments(engine, "rocm", root, root / "engine-data", 0); });
    rejects("Swift Q2_0 is not falsely advertised", [] {
        strata::model_checkpoint("ukisai/Swift-1.5-Qwen3.8-Flash-Next-GSQ-RCO-GGUF:Q2_0");
    });
    ModelInfo nvfp4;
    nvfp4.recipe = "strata";
    nvfp4.checkpoints["main"] = ready.repository + ":" + ready.quant;
    const json amd = {{"recipes", {{"strata", {{"backends", {{"rocm", {{"state", "installable"}}}}}}}}}};
    auto mixed = amd;
    mixed["recipes"]["strata"]["backends"]["cuda"] = {{"state", "installable"}};
    check("Catalog filters NVFP4 on AMD-only hosts", !strata::ops()->model_support_error(nvfp4, amd).empty());
    check("Catalog retains NVFP4 when NVIDIA is also present", strata::ops()->model_support_error(nvfp4, mixed).empty());
    models::BackendAvailability availability;
    availability.models["builtin.nvfp4"].unsupported_reason =
        strata::ops()->model_support_error(nvfp4, amd);
    auto compatible = nvfp4;
    compatible.checkpoints["main"] = flash.repository + ":" + flash.quant;
    const auto filtered = models::filter_hardware(
        {{"builtin.nvfp4", nvfp4}, {"builtin.flash", compatible}}, availability);
    check("Model-specific incompatibility survives host filtering",
        filtered.visible.count("builtin.nvfp4") == 0 &&
        filtered.visible.count("builtin.flash") == 1 && filtered.rejected.size() == 1 &&
        filtered.rejected.front().reason == availability.models.at("builtin.nvfp4").unsupported_reason);
    availability.models["builtin.nvfp4"].unsupported_reason =
        strata::ops()->model_support_error(nvfp4, mixed);
    check("Model-specific host filtering retains a compatible engine",
        models::filter_hardware({{"builtin.nvfp4", nvfp4}}, availability).visible.size() == 1);
    availability.filtering_disabled = true;
    availability.models["builtin.nvfp4"].unsupported_reason =
        strata::ops()->model_support_error(nvfp4, amd);
    check("Disabling host filtering preserves the model-specific bypass",
        models::filter_hardware({{"builtin.nvfp4", nvfp4}}, availability).visible.size() == 1);
    RecipeOptions wrong("strata", {{"strata_backend", "rocm"}});
    rejects("Explicit incompatible model/backend pair is rejected", [&] { strata::ops()->resolve_runtime_options(nvfp4, wrong); });
    auto models = json::object();
    models["new-owner/new-model"] = metadata.at("models").at(flash.repository);
    auto& model = models["new-owner/new-model"];
    model["quants"] = {{"CUSTOM", {"shards/a.gguf", "shards/b.gguf", "shards/c.gguf"}}};
    const auto checkpoint = strata::parse_model_checkpoint("new-owner/new-model:CUSTOM", models);
    check("Repository and shard count are metadata-driven", checkpoint.repository == "new-owner/new-model" &&
        checkpoint.revision == "main" && checkpoint.files.size() == 3 &&
        checkpoint.directory().parent_path().parent_path().filename() == "models--new-owner--new-model");
    const auto folder = root / "metadata-fixture";
    fs::create_directories(folder);
    for (const auto& file : checkpoint.files) std::ofstream(folder / fs::path(file).filename()) << "fixture";
    std::ofstream(folder / "expert-profile.bin") << "abc";
    auto legacy_model_identity = checkpoint.identity();
    legacy_model_identity["release"] = "previous-release";
    legacy_model_identity["expert_profile"] = checkpoint.profile_file;
    strata::record_downloads(folder, "lemonade-model.json", legacy_model_identity,
        {"a.gguf", "b.gguf", "c.gguf", "expert-profile.bin"});
    check("Matching model provenance is recognized", checkpoint.downloads_present(folder));
    auto changed = checkpoint;
    changed.revision = "another-revision";
    check("Revision upgrades invalidate downloaded state", !changed.downloads_present(folder));
    changed = checkpoint;
    check("Legacy release-scoped receipts preserve downloaded weights",
        changed.downloads_present(folder) && !changed.identity().contains("release") &&
        !changed.identity().contains("expert_profile"));
    auto changed_profile = changed;
    changed_profile.profile_file = "data/another-profile.bin";
    check("Profile source changes preserve weights", changed_profile.downloads_present(folder));
    changed = checkpoint;
    changed.files.push_back("shards/d.gguf");
    check("Shard list upgrades invalidate downloaded state", !changed.downloads_present(folder));
    fs::resize_file(folder / "expert-profile.bin", 1);
    check("Old per-model profiles do not affect downloaded weights", checkpoint.downloads_present(folder));
    fs::resize_file(folder / "a.gguf", 1);
    check("Truncated model weights still invalidate downloaded state", !checkpoint.downloads_present(folder));
    model["quants"]["CUSTOM"] = {"../outside.gguf"};
    rejects("Reject shard traversal in metadata", [&] { strata::parse_model_checkpoint("new-owner/new-model:CUSTOM", models); });
    model["quants"]["CUSTOM"] = {"a/shared.gguf", "b/shared.gguf"};
    rejects("Reject shard basename collisions", [&] { strata::parse_model_checkpoint("new-owner/new-model:CUSTOM", models); });
    model["quants"]["CUSTOM"] = {"a/shared.gguf", "b/SHARED.gguf"};
    rejects("Reject cross-platform case collisions", [&] { strata::parse_model_checkpoint("new-owner/new-model:CUSTOM", models); });
    model["quants"]["CUSTOM"] = {"pack"};
    rejects("Reject managed filename collisions", [&] { strata::parse_model_checkpoint("new-owner/new-model:CUSTOM", models); });
    model["quants"]["CUSTOM"] = {"valid.gguf"};
    model["expert_profile"] = "../outside";
    rejects("Reject profile source traversal", [&] { strata::parse_model_checkpoint("new-owner/new-model:CUSTOM", models); });
}

void test_native_transport(const fs::path& pack, const fs::path& executable) {
    ServerCommand command;
    command.program = utils::path_to_utf8(executable);
    command.args = {"--engine-fixture", ""};
    command.env = {{"LEMONADE_STRATA_NATIVE_FIXTURE", "1"}};
    httplib::Server occupied;
    command.port = occupied.bind_to_any_port("127.0.0.1");
    check("Native transport test reserves an occupied port", command.port > 0);
    strata::StrataProcess process(pack / "tokenizer.gguf", "test-model");
    const auto host = process.start(command);
    check("Native engine needs no HTTP host or free port", host.empty() && !process.uses_http() && process.running());
    const auto result = process.request({{"messages", {{{"role", "user"}, {"content", "Hello"}}}},
        {"max_tokens", 3}}, false, [] { return false; });
    check("Pipe inference works without any HTTP frontend", result["choices"][0]["message"]["content"] == "Hel");
    process.stop();
    occupied.stop();

    command.args.back() = "startup-delay";
    const auto started = std::chrono::steady_clock::now();
    strata::StrataProcess loading(pack / "tokenizer.gguf", "test-model", std::nullopt, [&] {
        return std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(100);
    });
    rejects("Native startup honors load cancellation", [&] { loading.start(command); });
    check("Cancelled startup reaps its engine promptly", !loading.running() &&
        std::chrono::steady_clock::now() - started < std::chrono::seconds(3));

    command.args.back() = "";
    auto encoder = command;
    encoder.args = {"--vision-fixture", "startup-delay"};
    const auto vision_started = std::chrono::steady_clock::now();
    strata::StrataProcess vision_loading(pack / "tokenizer.gguf", "test-model",
        strata::VisionCommand{encoder, 300}, [&] {
            return std::chrono::steady_clock::now() - vision_started >= std::chrono::milliseconds(100);
        });
    rejects("Vision startup honors load cancellation", [&] { vision_loading.start(command); });
    check("Cancelled vision startup returns promptly without starting inference",
        !vision_loading.running() && std::chrono::steady_clock::now() - vision_started < std::chrono::seconds(3));
}

void test_http(const fs::path& pack, const fs::path& executable) {
    const json request = {{"model", "test-model"}, {"messages", {{{"role", "user"}, {"content", "Hello"}}}},
                          {"max_tokens", 32}, {"temperature", 0}};
    MockStrataServer server;
    server.load_fixture(pack / "tokenizer.gguf", executable);
    const auto answer = server.chat_completion(request);
    check("Native pipe non-streaming chat", answer["choices"][0]["message"]["content"] == "Hello native");
    check("Native usage accounting", answer["usage"]["completion_tokens"] == 12);
    check("Native engine timings reach non-streaming chat and benchmark usage",
        answer["timings"]["prompt_ms"] == 1 && answer["timings"]["predicted_ms"] == 1 &&
        answer["timings"]["predicted_per_second"] == 12000 &&
        answer["usage"]["prefill_duration_ttft"] == 0.001 &&
        answer["usage"]["decoding_speed_tps"] == 12000 &&
        answer["timings"]["draft_n"] == 0 && answer["timings"]["draft_n_accepted"] == 0);
    for (const bool responses : {false, true}) {
        const std::string endpoint = responses ? "/v1/responses" : "/v1/chat/completions";
        auto payload = responses ? json{{"input", "Hello"}, {"max_output_tokens", 32}} : request;
        payload["stream"] = true;
        if (!responses) payload["stream_options"] = {{"include_usage", false}};
        std::string output;
        bool done = false;
        int callbacks = 0;
        StreamingProxy::TelemetryData telemetry;
        httplib::DataSink sink;
        sink.write = [&](const char* data, size_t size) { output.append(data, size); return true; };
        sink.is_writable = [] { return true; };
        sink.done = [&] { done = true; };
        server.forward_streaming_request(endpoint, payload.dump(), sink, true, 0,
            [&](const auto& data) { telemetry = data; ++callbacks; });
        check("Direct streaming preserves telemetry independently of wire usage",
            done && callbacks == 1 && telemetry.output_tokens == 12 &&
            telemetry.tokens_per_second == 12000 && telemetry.error_message.empty());
        if (!responses) check("Direct usage opt-out does not leak usage chunks", output.find("\"usage\"") == std::string::npos);
        output.clear();
        callbacks = 0;
        done = false;
        server.forward_streaming_request(endpoint, "{invalid", sink, true, 0,
            [&](const auto& data) { telemetry = data; ++callbacks; });
        check("Invalid direct streams report errors and telemetry once",
            done && callbacks == 1 && !telemetry.error_message.empty() &&
            output.find(ErrorType::INVALID_REQUEST) != std::string::npos);
        if (responses) check("Responses validation errors retain named SSE events", output.rfind("event: error\n", 0) == 0);
    }
    auto stream = request;
    stream["stream"] = true;
    stream["stream_options"] = {{"include_usage", true}};
    std::string sse;
    const auto result = utils::HttpClient::post_stream(server.url() + "/v1/chat/completions", stream.dump(),
        [&](const char* data, size_t size) { sse.append(data, size); return true; },
        {{"Content-Type", "application/json"}}, 10, nullptr, utils::HttpSecurityPolicy::TrustedLoopback);
    std::string text;
    std::istringstream events(sse);
    std::string line;
    bool streaming_timings = false;
    while (std::getline(events, line)) {
        if (line.rfind("data: {", 0) != 0) continue;
        const auto chunk = json::parse(line.substr(6));
        if (chunk.contains("usage")) streaming_timings =
            chunk["usage"]["decoding_speed_tps"] == 12000 && chunk["timings"]["prompt_ms"] == 1;
        if (chunk.contains("choices") && !chunk["choices"].empty()) {
            text += chunk["choices"][0]["delta"].value("content", "");
        }
    }
    check("Native pipe SSE chat", result.status_code == 200 && text == "Hello native" && sse.find("[DONE]") != std::string::npos);
    check("Streaming chat includes final engine timings", streaming_timings);
    for (const bool include_usage : {true, false}) {
        auto plain_stream = request;
        plain_stream["stream"] = true;
        if (!include_usage) plain_stream["stream_options"] = {{"include_usage", false}};
        std::string plain_sse;
        const auto plain_result = utils::HttpClient::post_stream(server.url() + "/v1/chat/completions", plain_stream.dump(),
            [&](const char* data, size_t size) { plain_sse.append(data, size); return true; },
            {{"Content-Type", "application/json"}}, 10, nullptr, utils::HttpSecurityPolicy::TrustedLoopback);
        std::istringstream plain_events(plain_sse);
        bool found_usage = false, found_timings = false, finished = false;
        while (std::getline(plain_events, line)) {
            if (line.rfind("data: {", 0) != 0) continue;
            const auto event = json::parse(line.substr(6));
            if (event.contains("usage")) {
                found_usage = event["usage"]["completion_tokens"] == 12 &&
                    event["usage"]["prompt_tokens"].is_number_integer();
            }
            if (event.contains("choices") && !event["choices"].empty() &&
                !event["choices"][0]["finish_reason"].is_null()) {
                finished = true;
                found_timings = event.contains("timings") && event["timings"]["prompt_n"].is_number_integer() &&
                    event["timings"]["cache_n"] == 0 && event["timings"]["prompt_per_second"].is_number() &&
                    event["timings"]["predicted_per_second"] == 12000;
            }
        }
        check("Official Strata benchmark receives final timings without include_usage",
            plain_result.status_code == 200 && finished && found_timings &&
            plain_sse.find("[DONE]") != std::string::npos);
        check("Strata streaming usage defaults on but honors explicit opt-out", found_usage == include_usage);
    }
    {
        MockStrataServer cached_server;
        cached_server.load_fixture(pack / "tokenizer.gguf", executable, "cached");
        const auto cached = cached_server.chat_completion(request);
        check("Cached prompt timing fields match official Strata accounting",
            cached["timings"]["cache_n"] == 5 &&
            cached["usage"]["prompt_tokens_details"]["cached_tokens"] == 5 &&
            cached["timings"]["prompt_n"].get<int>() + 5 == cached["usage"]["prompt_tokens"].get<int>());
    }
    const auto response = server.responses({{"model", "test-model"}, {"input", "Hello"}, {"max_output_tokens", 32}});
    check("Native Responses output", response["output"][0]["content"][0]["text"] == "Hello native");
    check("Responses preserve native engine timings", response["timings"] == answer["timings"]);
    sse.clear();
    utils::HttpClient::post_stream(server.url() + "/v1/responses",
        json{{"model", "test-model"}, {"input", "Hello"}, {"stream", true}, {"max_output_tokens", 32}}.dump(),
        [&](const char* data, size_t size) { sse.append(data, size); return true; },
        {{"Content-Type", "application/json"}}, 10, nullptr, utils::HttpSecurityPolicy::TrustedLoopback);
    check("Native Responses SSE", sse.find("response.output_text.delta") != std::string::npos &&
          sse.find("response.completed") != std::string::npos);
    check_response_stream(response_stream(server, {{"input", "Hello"}, {"max_output_tokens", 32}}));
    const auto incomplete = response_stream(server, {{"input", "Hello"}, {"max_output_tokens", 3}});
    check_response_stream(incomplete);
    check("Responses truncation is incomplete", incomplete.back()["type"] == "response.incomplete" &&
          incomplete.back()["response"]["output"][0]["status"] == "incomplete");
    auto stopped = request;
    stopped["stop"] = " native";
    check("Stop string is withheld", server.chat_completion(stopped)["choices"][0]["message"]["content"] == "Hello");
    auto invalid = request;
    invalid["top_k"] = 65;
    const auto bad = utils::HttpClient::post(server.url() + "/v1/chat/completions", invalid.dump(),
        {{"Content-Type", "application/json"}}, 10, utils::HttpSecurityPolicy::TrustedLoopback);
    check("Invalid sampling is HTTP 400", bad.status_code == 400);
    invalid["top_k"] = 1;
    invalid["max_tokens"] = 8192;
    check("Context overflow is HTTP 400", utils::HttpClient::post(server.url() + "/v1/chat/completions", invalid.dump(),
        {{"Content-Type", "application/json"}}, 10, utils::HttpSecurityPolicy::TrustedLoopback).status_code == 400);
    invalid = request;
    invalid["messages"][0]["content"] = {{{"type", "image_url"}, {"image_url", {{"url", "https://invalid.test/image"}}}}};
    check("Multimodal input is HTTP 400", utils::HttpClient::post(server.url() + "/v1/chat/completions", invalid.dump(),
        {{"Content-Type", "application/json"}}, 10, utils::HttpSecurityPolicy::TrustedLoopback).status_code == 400);
    auto parallel = std::async(std::launch::async, [&] { return server.chat_completion(request); });
    check("Concurrent request completes", server.chat_completion(request)["choices"][0]["message"]["content"] == "Hello native");
    check("Queued request completes", parallel.get()["choices"][0]["message"]["content"] == "Hello native");
    server.unload();

    MockStrataServer slow;
    slow.load_fixture(pack / "tokenizer.gguf", executable, "slow");
    const auto original_pid = slow.get_process_id();
    {
        int writes = 0, callbacks = 0;
        bool done = false;
        StreamingProxy::TelemetryData telemetry;
        httplib::DataSink sink;
        sink.write = [&](const char*, size_t) { return ++writes < 3; };
        sink.is_writable = [] { return true; };
        sink.done = [&] { done = true; };
        auto payload = request;
        payload["max_tokens"] = 1000;
        slow.forward_streaming_request("/v1/chat/completions", payload.dump(), sink, true, 0,
            [&](const auto& data) { telemetry = data; ++callbacks; });
        check("Failed direct sink writes cancel generation and report disconnect telemetry",
            done && writes == 3 && callbacks == 1 &&
            telemetry.error_message == "Client disconnected during stream");
        check("Direct cancellation retains the native engine PID", slow.get_process_id() == original_pid && slow.is_backend_alive());
    }
    std::atomic<bool> cancel{false};
    auto pending = std::async(std::launch::async, [&] {
        try {
            utils::HttpClient::post(slow.url() + "/v1/chat/completions",
                json{{"messages", request["messages"]}, {"max_tokens", 1000}}.dump(),
                {{"Content-Type", "application/json"}}, 10, utils::HttpSecurityPolicy::TrustedLoopback,
                {&cancel, nullptr});
            return false;
        } catch (const utils::HttpClientCancellationException&) { return true; }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    cancel = true;
    check("Non-streaming transfer cancellation", pending.get());
    const auto started = std::chrono::steady_clock::now();
    auto short_request = request;
    short_request["max_tokens"] = 3;
    const auto next = slow.chat_completion(short_request);
    check("Disconnect sends STOP without restarting engine",
          next["choices"][0]["message"]["content"] == "Hel" &&
          std::chrono::steady_clock::now() - started < std::chrono::seconds(3));
    int chunks = 0;
    auto cancelled_stream = request;
    cancelled_stream["stream"] = true;
    cancelled_stream["max_tokens"] = 1000;
    try {
        utils::HttpClient::post_stream(slow.url() + "/v1/chat/completions", cancelled_stream.dump(),
            [&](const char*, size_t) { return ++chunks < 3; },
            {{"Content-Type", "application/json"}}, 10, nullptr, utils::HttpSecurityPolicy::TrustedLoopback);
    } catch (const utils::HttpClientCancellationException&) {}
    const auto after_stream = std::chrono::steady_clock::now();
    check("Streaming disconnect drains before reusing the engine", chunks >= 3 &&
          slow.chat_completion(short_request)["choices"][0]["message"]["content"] == "Hel" &&
          std::chrono::steady_clock::now() - after_stream < std::chrono::seconds(3));
    slow.unload();

    MockStrataServer prefill;
    prefill.load_fixture(pack / "tokenizer.gguf", executable, "prefill");
    cancel = false;
    auto prefilling = std::async(std::launch::async, [&] {
        try {
            utils::HttpClient::post(prefill.url() + "/v1/chat/completions",
                json{{"messages", request["messages"]}, {"max_tokens", 1000}}.dump(),
                {{"Content-Type", "application/json"}}, 10, utils::HttpSecurityPolicy::TrustedLoopback,
                {&cancel, nullptr});
            return false;
        } catch (const utils::HttpClientCancellationException&) { return true; }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    cancel = true;
    check("Prefill transfer is aborted", prefilling.get());
    const auto after_prefill = std::chrono::steady_clock::now();
    check("Prefill disconnect sends STOP and releases queued inference",
          prefill.chat_completion(short_request)["choices"][0]["message"]["content"] == "Hel" &&
          std::chrono::steady_clock::now() - after_prefill < std::chrono::seconds(3));
    prefill.unload();

    MockStrataServer reasoning;
    reasoning.load_fixture(pack / "tokenizer.gguf", executable, "reasoning");
    auto reason_request = request;
    reason_request["max_tokens"] = 64;
    const auto reason = reasoning.chat_completion(reason_request)["choices"][0]["message"];
    check("Reasoning is separated from content",
          reason.value("reasoning_content", "") == "Check" && reason.value("content", "") == "Hello native");
    const auto reasoning_events = response_stream(reasoning, {{"input", "Hello"}, {"max_output_tokens", 64}});
    check_response_stream(reasoning_events);
    check("Responses reasoning precedes answer", reasoning_events.back()["response"]["output"][0]["summary"][0]["text"] == "Check" &&
          reasoning_events.back()["response"]["output"][1]["content"][0]["text"] == "Hello native");
    reasoning.unload();

    MockStrataServer tools;
    tools.load_fixture(pack / "tokenizer.gguf", executable, "tool");
    auto tool_request = request;
    tool_request["max_tokens"] = 256;
    const json weather = {
        {"name", "weather"}, {"description", "Weather lookup"},
        {"parameters", {{"type", "object"}, {"properties", {{"city", {{"type", "string"}}}}},
                        {"required", {"city"}}}}};
    tool_request["tools"] = json::array({{{"type", "function"}, {"function", weather}}});
    const auto tool = tools.chat_completion(tool_request);
    check("Native XML tool call is parsed", tool["choices"][0]["finish_reason"] == "tool_calls" &&
          tool["choices"][0]["message"]["tool_calls"][0]["function"]["name"] == "weather" &&
          json::parse(tool["choices"][0]["message"]["tool_calls"][0]["function"]["arguments"].get<std::string>())["city"] == "Rome");
    auto response_weather = weather;
    response_weather["type"] = "function";
    const auto tool_events = response_stream(tools, {{"input", "Weather?"}, {"max_output_tokens", 256},
        {"tools", json::array({response_weather})}});
    check_response_stream(tool_events);
    check("Responses streams function arguments", std::any_of(tool_events.begin(), tool_events.end(), [](const auto& e) {
        return e.value("type", "") == "response.function_call_arguments.delta";
    }) && tool_events.back()["response"]["output"][0]["type"] == "function_call");
    tools.unload();

    MockStrataServer utf8;
    utf8.load_fixture(pack / "tokenizer.gguf", executable, "utf8");
    check("UTF-8 split across engine tokens", utf8.chat_completion(request)["choices"][0]["message"]["content"] == "H\xc3\xa9");
    auto truncated = request;
    truncated["max_tokens"] = 2;
    check("Truncated UTF-8 is replaced safely", utf8.chat_completion(truncated)["choices"][0]["message"]["content"] == "H\xef\xbf\xbd");
    utf8.unload();

    MockStrataServer broken;
    broken.load_fixture(pack / "tokenizer.gguf", executable, "bad-token");
    check("Malformed engine output is HTTP 502", utils::HttpClient::post(broken.url() + "/v1/chat/completions", request.dump(),
        {{"Content-Type", "application/json"}}, 10, utils::HttpSecurityPolicy::TrustedLoopback).status_code == 502);
    const auto retry = broken.chat_completion(request);
    check("Dead native engine becomes a retryable watchdog reset",
        broken.was_watchdog_triggered() && retry.dump().find("backend_watchdog_reset") != std::string::npos);
    broken.unload();

    MockStrataServer unloading;
    unloading.load_fixture(pack / "tokenizer.gguf", executable, "prefill");
    auto active = std::async(std::launch::async, [&] {
        auto payload = request;
        payload["max_tokens"] = 1000;
        return unloading.chat_completion(payload);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (unloading.get_backend_health_state() != "busy" && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check("Native request is tracked as busy", unloading.get_backend_health_state() == "busy");
    unloading.unload();
    check("Concurrent unload preserves the active process object's lifetime", active.get().contains("error") &&
        unloading.get_process_id() == 0 && unloading.get_backend_health_state() == "stopped");
}

void test_vision(const fs::path& pack, const fs::path& executable, const fs::path& root) {
    const auto temp = root / "vision workspace with spaces";
    fs::create_directory(temp);
    struct Environment {
        std::vector<std::pair<std::string, std::string>> saved;
        explicit Environment(const fs::path& directory) {
            for (const auto* key : {"TEMP", "TMP", "TMPDIR"}) {
                saved.emplace_back(key, utils::get_environment_variable_utf8(key));
                utils::set_environment_variable_utf8(key, utils::path_to_utf8(directory));
            }
        }
        ~Environment() {
            for (const auto& [key, value] : saved) {
                try {
                    if (value.empty()) utils::unset_environment_variable_utf8(key);
                    else utils::set_environment_variable_utf8(key, value);
                } catch (const std::exception& e) {
                    std::cerr << "Environment restoration failed: " << e.what() << "\n";
                    ++failures;
                }
            }
        }
    } environment(temp);
    const auto picture = [](const std::string& data) {
        return json{{"type", "image_url"}, {"image_url",
            {{"url", "data:image/png;base64," + utils::JsonUtils::base64_encode(data)}}}};
    };
    const auto request = [&](const json& parts) {
        return json{{"messages", {{{"role", "user"}, {"content", parts}}}}, {"max_tokens", 3}};
    };
    const auto trace = root / "vision-cwd.txt";
    fs::path workspace;
    {
        MockStrataServer server;
        server.load_fixture(pack / "tokenizer.gguf", executable, "", std::string(), trace);
        std::ifstream input(trace);
        std::string cwd;
        std::getline(input, cwd);
        if (!input || cwd.empty()) throw std::runtime_error("Vision fixture did not record its working directory");
        workspace = utils::path_from_utf8(cwd);
        check("Vision pipe filenames work under spaces and Unicode", fs::equivalent(workspace.parent_path(), temp));
        auto body = request(json::array({{{"type", "text"}, {"text", "Describe this"}}, picture("first image")}));
        check("Chat image features reach GENI with expanded image tokens",
            server.chat_completion(body)["choices"][0]["message"]["content"] == "Hel");
        check("Repeated images reuse their encoded features",
            server.chat_completion(body)["choices"][0]["message"]["content"] == "Hel");
        body["messages"][0]["content"] = json::array({picture("second image"),
            {{"type", "text"}, {"text", "Compare <|vision_start|><|image_pad|><|vision_end|> with"}},
            picture("first image"), picture("second image")});
        check("Multiple ordered images and literal image markers remain distinct",
            server.chat_completion(body)["choices"][0]["message"]["content"] == "Hel");
        check("Request embeddings and raw image bytes are removed after inference",
            !fs::exists(workspace / "request.sve") && !fs::exists(workspace / "input.img"));
        json response = {{"input", {{{"role", "user"}, {"content", json::array({
            {{"type", "input_text"}, {"text", "What is shown?"}},
            {{"type", "input_image"}, {"image_url", picture("first image")["image_url"]["url"]}}})}}}},
            {"max_output_tokens", 3}};
        check("Responses accepts image input",
            server.responses(response)["output"][0]["content"][0]["text"] == "Hel");
        check_response_stream(response_stream(server, response));
        auto invalid = request(json::array({picture("invalid")}));
        const auto post = [&](const json& value) {
            return utils::HttpClient::post(server.url() + "/v1/chat/completions", value.dump(),
                {{"Content-Type", "application/json"}}, 10, utils::HttpSecurityPolicy::TrustedLoopback);
        };
        check("Unreadable images return HTTP 400", post(invalid).status_code == 400);
        invalid["messages"][0]["content"][0]["image_url"]["url"] = "data:image/png;base64,!!!!";
        check("Malformed base64 returns HTTP 400", post(invalid).status_code == 400);
        invalid["messages"][0]["content"][0]["image_url"]["url"] = "file:///private.png";
        check("Image requests cannot read server-local files", post(invalid).status_code == 400);
        invalid["messages"][0]["content"][0]["image_url"]["url"] = "http://example.test/image";
        check("Image downloads require HTTPS", post(invalid).status_code == 400);
        invalid = request(json::array({picture("third image")}));
        invalid["max_tokens"] = 8192;
        check("Image token expansion participates in context limits", post(invalid).status_code == 400);
        check("Rejected image requests leave no combined embeddings", !fs::exists(workspace / "request.sve"));
        invalid = request(json::array());
        for (int i = 0; i < 65; ++i) invalid["messages"][0]["content"].push_back(picture("first image"));
        check("Image count is bounded before encoding", post(invalid).status_code == 400);
        const auto pid = server.get_process_id();
        check("Invalid images do not reset the inference engine",
            server.chat_completion(body)["choices"][0]["message"]["content"] == "Hel" && server.get_process_id() == pid);
        server.unload();
    }
    check("Vision workspace and encoded cache are deleted on unload", !fs::exists(workspace));
    {
        MockStrataServer corrupt;
        corrupt.load_fixture(pack / "tokenizer.gguf", executable, "", std::string("bad-embedding"));
        const auto error = corrupt.chat_completion(request(json::array({picture("image")})));
        check("Invalid encoder output is an explicit backend error", error.contains("error") &&
            error["error"]["message"].get<std::string>().find("Invalid or oversized Strata vision embeddings") != std::string::npos);
    }
    {
        ServerCommand engine;
        engine.program = utils::path_to_utf8(executable);
        engine.args = {"--engine-fixture", ""};
        engine.env = {{"LEMONADE_STRATA_NATIVE_FIXTURE", "1"}};
        auto encoder = engine;
        encoder.args = {"--vision-fixture", "slow"};
        strata::StrataProcess process(pack / "tokenizer.gguf", "vision-test", strata::VisionCommand{encoder, 300});
        process.start(engine);
        std::atomic<bool> cancelled{false};
        const auto future_request = [&] {
            try { process.request(request(json::array({picture("slow image")})), false, [&] { return cancelled.load(); }); }
            catch (const std::runtime_error&) { return true; }
            return false;
        };
        auto active = std::async(std::launch::async, future_request);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const auto start = std::chrono::steady_clock::now();
        cancelled = true;
        check("Client cancellation aborts image encoding without blocking",
            active.wait_for(std::chrono::seconds(1)) == std::future_status::ready && active.get() &&
            std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
        check("Vision cancellation preserves the native inference process", process.running());
        cancelled = false;
        check("Text requests work after vision cancellation",
            process.request({{"messages", {{{"role", "user"}, {"content", "Hi"}}}}, {"max_tokens", 3}},
                false, [] { return false; })["choices"][0]["message"]["content"] == "Hel");
        active = std::async(std::launch::async, future_request);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        process.stop();
        check("Concurrent unload stops the reader and preserves request lifetime", active.get() && !process.running());
    }
    {
        MockStrataServer broken;
        rejects("Vision startup failures fail the load explicitly", [&] {
            broken.load_fixture(pack / "tokenizer.gguf", executable, "", std::string("startup-error"));
        });
    }
    {
        ServerCommand engine;
        engine.program = utils::path_to_utf8(executable);
        engine.args = {"--engine-fixture", ""};
        engine.env = {{"LEMONADE_STRATA_NATIVE_FIXTURE", "1"}};
        strata::StrataProcess process(pack / "tokenizer.gguf", "callback-test");
        process.start(engine);
        httplib::DataSink sink;
        sink.write = [](const char*, size_t) { return true; };
        sink.done = [] {};
        bool completed = false;
        process.stream({{"messages", {{{"role", "user"}, {"content", "Hi"}}}}, {"max_tokens", 3}},
            false, sink, [] { return false; }, [&](const json& result) {
                completed = !result.contains("error");
                process.stop();
            });
        check("Completion callbacks run outside the generation mutex", completed && !process.running());
    }
}

void test_release_data(const fs::path& root, const std::string& backend) {
    const auto files = strata::engine_data_files();
    const auto tag = BackendUtils::get_backend_version("strata", backend);
    const auto engine = strata::engine_release();
    auto binary = strata::spec()->binary;
#ifdef _WIN32
    binary += ".exe";
#endif
    std::ofstream(root / binary) << "fixture";
    strata::ops()->prepare_install(utils::path_to_utf8(root), backend, nullptr);
    const auto directory = root / "lemonade-data";
    for (const auto& file : files) {
        const auto path = directory / utils::path_from_utf8(file);
        check("Official release data staged beside engine: " + engine.data_url(tag, file),
            fs::is_regular_file(path) && fs::file_size(path) > 0);
    }
    check("Production install hook stages verified release data",
        strata::engine_data_present(directory, backend));
}

void test_engine_data(const fs::path& root) {
    const auto folder = root / "engine-install";
    const auto staging = root / "engine-install.staging";
    fs::create_directories(folder);
    fs::create_directories(staging);
    const auto read = [](const fs::path& path) {
        std::ifstream input(path);
        return std::string(std::istreambuf_iterator<char>(input), {});
    };
    std::ofstream(folder / "strata") << "old-engine";
    std::ofstream(staging / "strata") << "new-engine";
    strata::prepare_engine_data(folder / "lemonade-data", "rocm", [](const std::string&, const fs::path& path) {
        std::ofstream(path) << "old-data";
    });
    int downloads = 0;
    rejects("Failed engine data download aborts staged preparation", [&] {
        strata::prepare_engine_data(staging / "lemonade-data", "rocm", [&](const std::string&, const fs::path& path) {
            if (++downloads == 2) throw std::runtime_error("network failure");
            std::ofstream(path) << "new-data";
        });
    });
    check("Failed engine update preserves installed engine and data",
        read(folder / "strata") == "old-engine" && strata::engine_data_present(folder / "lemonade-data", "rocm") &&
        !strata::engine_data_present(staging / "lemonade-data", "rocm"));
    downloads = 0;
    const auto tag = BackendUtils::get_backend_version("strata", "rocm");
    strata::prepare_engine_data(staging / "lemonade-data", "rocm", [&](const std::string& url, const fs::path& path) {
        ++downloads;
        check("Engine data download uses the selected release tag", url.find("/" + tag + "/") != std::string::npos);
        std::ofstream(path) << "new-data";
    });
    check("Engine data includes every profile and vocabulary once", downloads == strata::engine_data_files().size());
    check("Staged engine data has a complete receipt", strata::engine_data_present(staging / "lemonade-data", "rocm"));
    downloads = 0;
    strata::prepare_engine_data(staging / "lemonade-data", "rocm", [&](const std::string&, const fs::path& path) {
        ++downloads;
        std::ofstream(path) << "new-data";
    });
    check("Engine preparation always fetches every file even with a matching receipt",
        downloads == strata::engine_data_files().size());
    check("Engine and auxiliary data are promoted together",
        !commit_staged_install(utils::path_to_utf8(staging), utils::path_to_utf8(folder), "strata").empty() &&
        read(folder / "strata") == "new-engine" && strata::engine_data_present(folder / "lemonade-data", "rocm"));
    const auto data_file = folder / "lemonade-data" / *strata::engine_data_files().begin();
    check("Promoted auxiliary data contains new bytes", read(data_file) == "new-data");
    fs::resize_file(data_file, 1);
    check("Truncated engine-owned data is rejected", !strata::engine_data_present(folder / "lemonade-data", "rocm"));
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--engine-fixture") return fixture_engine(argc >= 3 ? argv[2] : "");
    if (argc >= 2 && std::string(argv[1]) == "--vision-fixture") return fixture_vision(argc >= 3 ? argv[2] : "");
    const auto root = fs::temp_directory_path() / utils::path_from_utf8("lemon-strata-native-\xe2\x98\x83-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    utils::set_models_dir(utils::path_to_utf8(root));
    try {
        if (argc == 3 && std::string(argv[1]) == "--release-data") {
            test_release_data(root, argv[2]);
            fs::remove_all(root);
            return failures ? 1 : 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--vocabulary-fixture") {
            strata::prepare_vocabulary(utils::path_from_utf8(argv[2]), root / "pack" / "tokenizer.gguf");
            MockStrataServer server;
            server.load_fixture(root / "pack" / "tokenizer.gguf", fs::absolute(utils::path_from_utf8(argv[0])));
            const auto result = server.chat_completion({{"model", "test-model"},
                {"messages", {{{"role", "user"}, {"content", "Hello"}}}}, {"max_tokens", 3}});
            const auto& message = result.at("choices").at(0).at("message");
            check("Real GGUF metadata tokenizes and serves through the native adapter",
                !(message.value("content", std::string()) + message.value("reasoning_content", std::string())).empty() &&
                result.at("usage").at("completion_tokens") == 3);
            server.unload();
            fs::remove_all(root);
            return failures ? 1 : 0;
        }
        test_assets();
        test_release_assets();
        test_cache_isolation();
        test_memory();
        test_metadata(root);
        test_engine_data(root);
        const std::vector<fs::path> shards = {root / "fixture-00001-of-00002.gguf", root / "fixture-00002-of-00002.gguf"};
        make_shards(shards);
        const auto pack = root / "pack";
        strata::prepare_pack(shards, pack);
        check("Native GGUF pack is complete", strata::pack_present(pack));
        const auto index = [&] { std::ifstream in(pack / "index.txt"); return std::string(std::istreambuf_iterator<char>(in), {}); }();
        check("Quantized PLE key stays native", index.find("blk.1.ple_key.weight 0 0 0 0") != std::string::npos);
        check("Dense tensor index uses v3", index.find("# strata pack index v3") == 0);
        test_pack_layout(shards, pack);
        test_native_transport(pack, fs::absolute(utils::path_from_utf8(argv[0])));
        test_http(pack, fs::absolute(utils::path_from_utf8(argv[0])));
        test_vision(pack, fs::absolute(utils::path_from_utf8(argv[0])), root);
        test_model_formats(root);
        auto* source = gguf_init_from_file(utils::path_to_utf8(shards[0]).c_str(), {true, nullptr});
        const auto tensor = gguf_find_tensor(source, "blk.0.ssm_alpha.weight");
        const auto offset = gguf_get_data_offset(source) + gguf_get_tensor_offset(source, tensor);
        gguf_free(source);
        const auto replace = [&](float value) {
            std::fstream file(shards[0], std::ios::binary | std::ios::in | std::ios::out);
            file.seekp(static_cast<std::streamoff>(offset));
            file.write(reinterpret_cast<const char*>(&value), sizeof(value));
        };
        replace(1.1f);
        rejects("Lossy BF16 preparation is rejected", [&] { strata::prepare_pack(shards, pack); });
        check("Failed conversion leaves no completion marker", !strata::pack_present(pack));
        replace(std::numeric_limits<float>::infinity());
        rejects("Compat-BF16 refuses non-finite dense weights", [&] { strata::prepare_pack(shards, pack, nullptr, true); });
        replace(1.0f);
        strata::prepare_pack(shards, pack);
        fs::resize_file(pack / "dense.bin", 1);
        check("Truncated prepared pack is rejected", !strata::pack_present(pack));
        rejects("Cancelled preparation fails explicitly", [&] {
            strata::prepare_pack(shards, pack, [](const auto&) { return false; });
        });
        check("Cancelled preparation leaves no completion marker", !strata::pack_present(pack));
        rejects("Missing GGUF shard fails preparation", [&] { strata::prepare_pack({root / "missing.gguf"}, pack); });
        rejects("Duplicate GGUF tensors fail preparation", [&] { strata::prepare_pack({shards[0], shards[0]}, pack); });
        test_model_context();
        ModelInfo info;
        info.recipe = "strata";
        info.checkpoints["main"] = "ISTA-DASLab/Qwen3.8-Flash-Next-GSQ-RCO-GGUF:Q2_0";
        CheckpointResolveContext context;
        context.checkpoint = info.checkpoint();
        const auto model = utils::path_from_utf8(strata::ops()->resolve_checkpoint_path(info, context)).parent_path().parent_path();
        fs::create_directories(model);
        std::ofstream(model / "fixture") << "managed";
        const auto sibling = model.parent_path() / "IQ3_S";
        fs::create_directories(sibling);
        std::ofstream(sibling / "fixture") << "keep";
        ModelManager manager;
        manager.register_user_model("strata-delete-fixture", {
            {"recipe", "strata"}, {"checkpoint", info.checkpoint()}
        });
        manager.delete_model("user.strata-delete-fixture");
        check("Backend-owned deletion through the model facade",
            !fs::exists(model) && !manager.model_exists("user.strata-delete-fixture"));
        check("Deleting one quant preserves siblings", !fs::exists(model) && fs::is_regular_file(sibling / "fixture"));
    } catch (const std::exception& e) {
        std::cerr << "FAIL unexpected exception: " << e.what() << "\n";
        ++failures;
    }
    fs::remove_all(root);
    return failures ? 1 : 0;
}

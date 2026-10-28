#include "lemon/backends/strata/strata_model.h"

#include "lemon/utils/http_client.h"
#include "lemon/utils/json_utils.h"
#include "lemon/utils/path_utils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <ggml.h>
#include <gguf.h>
#include <limits>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <type_traits>

#ifdef __APPLE__
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace lemon::backends::strata {
namespace {

using Gguf = std::unique_ptr<gguf_context, decltype(&gguf_free)>;
const std::regex expert_tensor_pattern("^blk\\.([0-9]+)\\.ffn_(gate|up|down)_exps\\.weight$");

struct Tensor {
    fs::path shard;
    uint64_t offset;
    uint64_t size;
    ggml_type type;
    std::vector<int64_t> shape;
    uint32_t dimensions = 4;
};

uint64_t align(uint64_t n) { return (n + 63) / 64 * 64; }

uint64_t aligned(uint64_t n, uint64_t alignment) {
    if (!alignment || (alignment & (alignment - 1)) || alignment > 65536 ||
        n > std::numeric_limits<uint64_t>::max() - alignment) {
        throw std::invalid_argument("Invalid Strata GGUF alignment or offset");
    }
    return (n + alignment - 1) / alignment * alignment;
}

struct GgufReader {
    std::ifstream input;
    uint64_t size;
    Gguf metadata{nullptr, gguf_free};
    std::vector<char> raw_metadata;
    std::map<std::string, std::pair<uint64_t, uint64_t>> entries;
    std::map<std::string, Tensor> tensors;

    template<class T> T number() {
        T value{};
        input.read(reinterpret_cast<char*>(&value), sizeof(value));
        if (!input) throw std::runtime_error("Truncated Strata GGUF header");
        return value;
    }
    std::string string() {
        const auto length = number<uint64_t>();
        if (length > 64 * 1024 * 1024 || length > size - position()) {
            throw std::invalid_argument("Invalid Strata GGUF string length");
        }
        std::string value(static_cast<size_t>(length), '\0');
        input.read(value.data(), static_cast<std::streamsize>(length));
        if (!input) throw std::runtime_error("Truncated Strata GGUF string");
        return value;
    }
    uint64_t position() {
        const auto at = input.tellg();
        if (at < 0 || static_cast<uint64_t>(at) > size) throw std::runtime_error("Invalid Strata GGUF position");
        return static_cast<uint64_t>(at);
    }
    void skip_value(uint32_t type, bool array = false) {
        if (type == GGUF_TYPE_STRING) { string(); return; }
        if (type == GGUF_TYPE_ARRAY && !array) {
            const auto item = number<uint32_t>();
            const auto count = number<uint64_t>();
            if (count > 10000000) throw std::invalid_argument("Strata GGUF metadata array is too large");
            for (uint64_t i = 0; i < count; ++i) skip_value(item, true);
            return;
        }
        static const std::map<uint32_t, uint64_t> sizes = {
            {0, 1}, {1, 1}, {2, 2}, {3, 2}, {4, 4}, {5, 4}, {6, 4}, {7, 1}, {10, 8}, {11, 8}, {12, 8},
        };
        const auto it = sizes.find(type);
        if (it == sizes.end() || it->second > size - position()) {
            throw std::invalid_argument("Invalid Strata GGUF metadata type or size");
        }
        input.seekg(static_cast<std::streamoff>(it->second), std::ios::cur);
    }
    explicit GgufReader(const fs::path& path, bool vocabulary_only = false)
        : input(path, std::ios::binary), size(fs::file_size(path)) {
        if (number<uint32_t>() != 0x46554747 || number<uint32_t>() != 3) {
            throw std::invalid_argument("Strata requires a GGUF v3 checkpoint");
        }
        const auto count = number<uint64_t>();
        const auto keys = number<uint64_t>();
        if (count > 100000 || keys > 100000) throw std::invalid_argument("Strata GGUF header is too large");
        uint64_t alignment = 32;
        for (uint64_t i = 0; i < keys; ++i) {
            const auto start = position();
            const auto key = string();
            const auto type = number<uint32_t>();
            if (key == "general.alignment") {
                if (type != GGUF_TYPE_UINT32) throw std::invalid_argument("Invalid GGUF alignment type");
                alignment = number<uint32_t>();
            } else skip_value(type);
            if (!entries.emplace(key, std::make_pair(start, position() - start)).second) {
                throw std::invalid_argument("Duplicate Strata GGUF metadata key: " + key);
            }
        }
        const auto meta_end = position();
        if (meta_end > 128 * 1024 * 1024) throw std::invalid_argument("Strata GGUF metadata is too large");
        std::vector<char> header(static_cast<size_t>(aligned(meta_end, alignment)), '\0');
        input.seekg(0);
        input.read(header.data(), static_cast<std::streamsize>(meta_end));
        raw_metadata.assign(header.begin(), header.begin() + static_cast<size_t>(meta_end));
        const uint64_t no_tensors = 0;
        std::memcpy(header.data() + 8, &no_tensors, sizeof(no_tensors));
        // Custom expert types are opaque to llama.cpp; only the vocabulary metadata goes through its reader.
        metadata.reset(gguf_init_from_buffer(header.data(), header.size(), {true, nullptr}));
        if (!metadata) throw std::invalid_argument("Invalid Strata GGUF metadata");
        if (vocabulary_only) return;
        for (uint64_t i = 0; i < count; ++i) {
            const auto name = string();
            const auto dims = number<uint32_t>();
            if (!dims || dims > 4) throw std::invalid_argument("Invalid Strata GGUF tensor dimensions");
            Tensor tensor{path, 0, 0, GGML_TYPE_F32, {1, 1, 1, 1}};
            tensor.dimensions = dims;
            uint64_t elements = 1;
            for (uint32_t d = 0; d < dims; ++d) {
                const auto n = number<uint64_t>();
                if (!n || n > static_cast<uint64_t>(INT64_MAX) / elements) {
                    throw std::invalid_argument("Invalid Strata GGUF tensor shape");
                }
                tensor.shape[d] = static_cast<int64_t>(n);
                elements *= n;
            }
            const auto type = number<uint32_t>();
            tensor.type = static_cast<ggml_type>(type);
            tensor.offset = number<uint64_t>();
            uint64_t block = 0, bytes = 0;
            if (type < GGML_TYPE_COUNT) {
                block = ggml_blck_size(tensor.type);
                bytes = ggml_type_size(tensor.type);
            } else {
                static const auto custom = utils::JsonUtils::load_from_file(
                    utils::get_resource_path("resources/strata.json")).at("quant_types");
                const auto key = std::to_string(type);
                if (!custom.contains(key)) throw std::invalid_argument("Unsupported Strata GGUF type " + key);
                block = custom.at(key).at("block").get<uint64_t>();
                bytes = custom.at(key).at("bytes").get<uint64_t>();
            }
            if (!block || !bytes || static_cast<uint64_t>(tensor.shape[0]) % block ||
                elements / block > std::numeric_limits<uint64_t>::max() / bytes) {
                throw std::invalid_argument("Invalid Strata GGUF tensor block geometry");
            }
            tensor.size = elements / block * bytes;
            if (!tensors.emplace(name, std::move(tensor)).second) throw std::invalid_argument("Duplicate GGUF tensor: " + name);
        }
        const auto data = aligned(position(), alignment);
        for (auto& [name, tensor] : tensors) {
            if (data > size || tensor.offset > size - data || tensor.size > size - data - tensor.offset) {
                throw std::runtime_error("Truncated GGUF tensor: " + name);
            }
            tensor.offset += data;
        }
    }
};

void report(const DownloadProgressCallback& progress, const std::string& file) {
    if (utils::g_download_cancelled.load()) throw std::runtime_error("Strata model preparation cancelled");
    if (progress) {
        DownloadProgress p;
        p.file = "Preparing Strata pack: " + file;
        if (!progress(p)) throw std::runtime_error("Strata model preparation cancelled");
    }
}

ggml_type required_form(const std::string& name, ggml_type stored) {
    const auto key = std::regex_replace(name, std::regex("^blk\\.[0-9]+\\."), "");
    const std::set<std::string> bf16 = {
        "ffn_gate_inp.weight", "ffn_gate_inp_shexp.weight",
        "hc_attn_down.weight", "hc_attn_up.weight", "hc_attn_inject.weight",
        "hc_ffn_down.weight", "hc_ffn_up.weight", "hc_ffn_inject.weight",
        "output_hc_down.weight", "output_hc_up.weight", "indexer.k_proj.weight", "indexer.q_proj.weight",
        "ssm_alpha.weight", "ssm_beta.weight", "ple_key.weight", "ple_value.weight",
    };
    const std::set<std::string> f32 = {
        "attn_q_norm.weight", "attn_k_norm.weight", "hc_attn_norm.weight", "hc_ffn_norm.weight",
        "output_hc_norm.weight", "indexer.q_norm.weight", "indexer.k_norm.weight",
        "ple_norm_conv.weight", "ple_norm_key.weight", "ple_norm_query.weight",
        "ssm_a", "ssm_conv1d.weight", "ssm_dt.bias", "ssm_norm.weight",
    };
    if (name == "blk.1.ple_key.weight" &&
        (stored == GGML_TYPE_Q2_0 || stored == GGML_TYPE_Q8_0 ||
         stored == GGML_TYPE_IQ3_XXS || stored == GGML_TYPE_IQ4_XS)) return stored;
    if (bf16.count(key)) return GGML_TYPE_BF16;
    if (f32.count(key)) return GGML_TYPE_F32;
    if (key == "ple_conv1d.weight") return GGML_TYPE_F16;
    return stored;
}

bool is_float(ggml_type type) {
    return type == GGML_TYPE_F32 || type == GGML_TYPE_F16 || type == GGML_TYPE_BF16;
}

void copy_tensor(const Tensor& tensor, ggml_type form, std::ofstream& out,
                 const DownloadProgressCallback& progress, const std::string& name, bool compat_bf16 = false) {
    std::ifstream in(tensor.shard, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(tensor.offset));
    if (compat_bf16 && form != tensor.type) {
        if (tensor.type >= GGML_TYPE_COUNT) throw std::invalid_argument("Cannot dequantize custom dense type: " + name);
        const auto* traits = ggml_get_type_traits(tensor.type);
        if (!traits->to_float && tensor.type != GGML_TYPE_F32) {
            throw std::invalid_argument("Cannot dequantize Strata tensor: " + name);
        }
        const auto width = tensor.shape[0];
        std::vector<char> row(ggml_row_size(tensor.type, width));
        std::vector<float> values(static_cast<size_t>(width));
        std::vector<uint16_t> converted(static_cast<size_t>(width));
        const uint64_t rows = tensor.size / row.size();
        for (uint64_t r = 0; r < rows; ++r) {
            if (r % std::max<size_t>(1, 1024 * 1024 / row.size()) == 0) report(progress, name);
            in.read(row.data(), static_cast<std::streamsize>(row.size()));
            if (!in) throw std::runtime_error("Truncated Strata tensor: " + name);
            if (tensor.type == GGML_TYPE_F32) std::memcpy(values.data(), row.data(), row.size());
            else traits->to_float(row.data(), values.data(), width);
            for (size_t i = 0; i < values.size(); ++i) {
                const auto value = values[i];
                if (!std::isfinite(value)) throw std::invalid_argument("Non-finite Strata weight: " + name);
                if (form == GGML_TYPE_BF16) {
                    const auto bf16 = ggml_fp32_to_bf16(value);
                    if (!std::isfinite(ggml_bf16_to_fp32(bf16))) throw std::invalid_argument("Strata BF16 overflow: " + name);
                    converted[i] = bf16.bits;
                } else if (form == GGML_TYPE_F16) {
                    converted[i] = ggml_fp32_to_fp16(value);
                    if (!std::isfinite(ggml_fp16_to_fp32(converted[i]))) throw std::invalid_argument("Strata F16 overflow: " + name);
                } else if (form != GGML_TYPE_F32) throw std::invalid_argument("Unsupported Strata dense form: " + name);
            }
            if (form == GGML_TYPE_F32 || (form == GGML_TYPE_F16 && tensor.type == GGML_TYPE_F32)) {
                out.write(reinterpret_cast<const char*>(values.data()), static_cast<std::streamsize>(values.size() * 4));
            } else {
                out.write(reinterpret_cast<const char*>(converted.data()), static_cast<std::streamsize>(converted.size() * 2));
            }
        }
        if (!out) throw std::runtime_error("Could not write Strata converted weights");
        return;
    }
    std::vector<char> buffer(1024 * 1024);
    uint64_t remaining = tensor.size;
    while (remaining) {
        report(progress, name);
        const auto n = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
        in.read(buffer.data(), static_cast<std::streamsize>(n));
        if (!in) throw std::runtime_error("Truncated Strata tensor: " + name);
        if (tensor.type == GGML_TYPE_F32 && form == GGML_TYPE_F16) {
            for (size_t i = 0; i < n; i += 4) {
                float value;
                std::memcpy(&value, buffer.data() + i, 4);
                if (!std::isfinite(value) || !std::isfinite(ggml_fp16_to_fp32(ggml_fp32_to_fp16(value)))) {
                    throw std::invalid_argument("Strata F16 overflow or non-finite weight: " + name);
                }
            }
        }
        if (form == tensor.type || (tensor.type == GGML_TYPE_F32 && form == GGML_TYPE_F16)) {
            out.write(buffer.data(), static_cast<std::streamsize>(n));
        } else if (tensor.type == GGML_TYPE_F32 && form == GGML_TYPE_BF16) {
            for (size_t i = 0; i < n; i += 4) {
                uint32_t bits;
                std::memcpy(&bits, buffer.data() + i, 4);
                if ((bits & 0xffff) || (bits & 0x7f800000) == 0x7f800000) {
                    throw std::runtime_error("Strata requires exactly representable BF16 weights: " + name);
                }
                const auto bf16 = static_cast<uint16_t>(bits >> 16);
                out.write(reinterpret_cast<const char*>(&bf16), 2);
            }
        } else if (form == GGML_TYPE_F32 && (tensor.type == GGML_TYPE_BF16 || tensor.type == GGML_TYPE_F16)) {
            for (size_t i = 0; i < n; i += 2) {
                uint16_t bits;
                std::memcpy(&bits, buffer.data() + i, 2);
                const float value = tensor.type == GGML_TYPE_BF16
                    ? ggml_bf16_to_fp32(ggml_bf16_t{bits}) : ggml_fp16_to_fp32(bits);
                out.write(reinterpret_cast<const char*>(&value), 4);
            }
        } else {
            throw std::runtime_error("Unsupported Strata tensor conversion: " + name);
        }
        if (!out) throw std::runtime_error("Could not write Strata dense weights");
        remaining -= n;
    }
}

template<class T> int64_t signed_value(const void* data) {
    T value{};
    std::memcpy(&value, data, sizeof(value));
    if constexpr (std::is_unsigned_v<T>) {
        if (value > static_cast<uint64_t>(INT64_MAX)) throw std::invalid_argument("Strata metadata integer overflow");
    }
    return static_cast<int64_t>(value);
}

int64_t metadata_integer(gguf_type type, const void* data) {
    switch (type) {
    case GGUF_TYPE_UINT8: return signed_value<uint8_t>(data);
    case GGUF_TYPE_INT8: return signed_value<int8_t>(data);
    case GGUF_TYPE_UINT16: return signed_value<uint16_t>(data);
    case GGUF_TYPE_INT16: return signed_value<int16_t>(data);
    case GGUF_TYPE_UINT32: return signed_value<uint32_t>(data);
    case GGUF_TYPE_INT32: return signed_value<int32_t>(data);
    case GGUF_TYPE_UINT64: return signed_value<uint64_t>(data);
    case GGUF_TYPE_INT64: return signed_value<int64_t>(data);
    default: throw std::invalid_argument("Strata Hadamard metadata requires integers");
    }
}

int64_t metadata_integer(const gguf_context* file, const char* key) {
    const auto id = gguf_find_key(file, key);
    if (id < 0) throw std::invalid_argument("Missing Strata Hadamard metadata: " + std::string(key));
    return metadata_integer(gguf_get_kv_type(file, id), gguf_get_val_data(file, id));
}

std::vector<std::string> metadata_strings(const gguf_context* file, const char* key) {
    const auto id = gguf_find_key(file, key);
    if (id < 0) return {};
    if (gguf_get_kv_type(file, id) != GGUF_TYPE_ARRAY || gguf_get_arr_type(file, id) != GGUF_TYPE_STRING) {
        throw std::invalid_argument("Invalid Strata string array: " + std::string(key));
    }
    std::vector<std::string> result;
    for (size_t i = 0; i < gguf_get_arr_n(file, id); ++i) result.emplace_back(gguf_get_arr_str(file, id, i));
    return result;
}

std::vector<int64_t> metadata_integers(const gguf_context* file, const char* key) {
    const auto id = gguf_find_key(file, key);
    if (id < 0 || gguf_get_kv_type(file, id) != GGUF_TYPE_ARRAY) {
        throw std::invalid_argument("Invalid Strata integer array: " + std::string(key));
    }
    const auto type = gguf_get_arr_type(file, id);
    static const std::map<gguf_type, size_t> widths = {
        {GGUF_TYPE_UINT8, 1}, {GGUF_TYPE_INT8, 1}, {GGUF_TYPE_UINT16, 2}, {GGUF_TYPE_INT16, 2},
        {GGUF_TYPE_UINT32, 4}, {GGUF_TYPE_INT32, 4}, {GGUF_TYPE_UINT64, 8}, {GGUF_TYPE_INT64, 8},
    };
    const auto found = widths.find(type);
    if (found == widths.end()) throw std::invalid_argument("Strata Hadamard requires an integer array");
    const auto width = found->second;
    const auto* data = static_cast<const char*>(gguf_get_arr_data(file, id));
    std::vector<int64_t> result;
    for (size_t i = 0; i < gguf_get_arr_n(file, id); ++i) result.push_back(metadata_integer(type, data + i * width));
    return result;
}

std::string hadamard_text(const gguf_context* file, const std::map<std::string, Tensor>& tensors, size_t layers) {
    if (gguf_find_key(file, "prism.hadamard.version") < 0) return "";
    const auto block = metadata_integer(file, "prism.hadamard.block_size");
    if (metadata_integer(file, "prism.hadamard.version") != 1 || block <= 1 || (block & (block - 1))) {
        throw std::invalid_argument("Unsupported Strata Hadamard version or block");
    }
    const auto text = [&](const char* key) -> std::string {
        const auto id = gguf_find_key(file, key);
        if (id < 0 || gguf_get_kv_type(file, id) != GGUF_TYPE_STRING) {
            throw std::invalid_argument("Invalid Strata Hadamard metadata: " + std::string(key));
        }
        return gguf_get_val_str(file, id);
    };
    const auto mode = text("prism.hadamard.sign_mode");
    if (text("prism.hadamard.transform") != "normalized-sylvester-walsh-hadamard" ||
        text("prism.hadamard.axis") != "input-last-dimension" || (mode != "identity" && mode != "explicit") ||
        !metadata_strings(file, "prism.hadamard.inverse_weight_names").empty()) {
        throw std::invalid_argument("Unsupported Strata Hadamard transform, axis, signs or inverse weights");
    }
    const auto names = metadata_strings(file, "prism.hadamard.weight_names");
    if (names.empty()) throw std::invalid_argument("Strata Hadamard weight list is empty");
    std::vector<int> roles(layers, 0);
    std::set<int64_t> widths;
    for (const auto& name : names) {
        std::smatch match;
        if (!std::regex_match(name, match, expert_tensor_pattern) || !tensors.count(name)) {
            throw std::invalid_argument("Unsupported Strata Hadamard weight: " + name);
        }
        const auto layer = static_cast<size_t>(std::stoul(match[1]));
        const auto bit = match[2] == "gate" ? 1 : match[2] == "up" ? 2 : 4;
        const auto& tensor = tensors.at(name);
        if (layer >= layers || (roles[layer] & bit) || tensor.type == GGML_TYPE_Q2_0 || tensor.shape[0] % block) {
            throw std::invalid_argument("Invalid Strata Hadamard expert or geometry: " + name);
        }
        roles[layer] |= bit;
        widths.insert(tensor.shape[0]);
    }
    std::ostringstream out;
    out << "# strata hadamard v1\nblock " << block << "\n";
    if (mode == "explicit") {
        const auto sign_widths = metadata_integers(file, "prism.hadamard.sign_widths");
        const auto signs = metadata_integers(file, "prism.hadamard.sign_values");
        std::set<int64_t> present;
        size_t offset = 0;
        for (const auto width : sign_widths) {
            if (width <= 0 || width % block || static_cast<uint64_t>(width) > signs.size() - offset ||
                !present.insert(width).second) throw std::invalid_argument("Invalid Strata Hadamard sign widths");
            out << "signs " << width;
            for (int64_t j = 0; j < width; ++j) {
                const auto sign = signs[offset++];
                if (sign != 1 && sign != -1) throw std::invalid_argument("Invalid Strata Hadamard sign value");
                out << " " << sign;
            }
            out << "\n";
        }
        if (offset != signs.size() || !std::includes(present.begin(), present.end(), widths.begin(), widths.end())) {
            throw std::invalid_argument("Incomplete Strata Hadamard sign vectors");
        }
    }
    for (size_t layer = 0; layer < roles.size(); ++layer) {
        const auto r = roles[layer];
        if (bool(r & 1) != bool(r & 2)) throw std::invalid_argument("Strata Hadamard gate/up must rotate together");
        if (r) out << "layer " << layer << " " << (r & 1) << " " << ((r >> 1) & 1) << " " << ((r >> 2) & 1) << "\n";
    }
    return out.str();
}

void write_vocabulary(gguf_context* vocab, const fs::path& destination) {
    for (const auto& key : {"tokenizer.ggml.tokens", "tokenizer.ggml.merges", "tokenizer.chat_template"}) {
        if (gguf_find_key(vocab, key) < 0) throw std::invalid_argument("Strata GGUF lacks " + std::string(key));
    }
    // This metadata-only artifact is tokenized as llama; no architecture-specific weights are loaded.
    gguf_set_val_str(vocab, "general.architecture", "llama");
    for (const auto& key : {"split.count", "split.no", "split.tensors.count"}) gguf_remove_key(vocab, key);
    fs::create_directories(destination.parent_path());
    if (!gguf_write_to_file(vocab, utils::path_to_utf8(destination).c_str(), true)) {
        throw std::runtime_error("Could not write Strata vocabulary");
    }
}

}

uint64_t model_expert_bytes(const std::vector<fs::path>& shards) {
    uint64_t bytes = 0;
    for (const auto& shard : shards) {
        GgufReader reader(shard);
        for (const auto& [name, tensor] : reader.tensors) {
            if (std::regex_match(name, expert_tensor_pattern)) {
                if (bytes > std::numeric_limits<uint64_t>::max() - tensor.size) {
                    throw std::invalid_argument("Strata tensor memory size overflow");
                }
                bytes += tensor.size;
            }
        }
    }
    return bytes;
}

void prepare_pack(const std::vector<fs::path>& shards, const fs::path& destination,
                  DownloadProgressCallback progress, bool compat_bf16) {
    if (shards.empty()) throw std::invalid_argument("Strata requires GGUF shards");
    fs::create_directories(destination);
    fs::remove(destination / "lemonade-native.json");
    std::map<std::string, Tensor> tensors;
    Gguf vocab(gguf_init_empty(), gguf_free);
    bool has_ple = false;
    for (size_t s = 0; s < shards.size(); ++s) {
        report(progress, utils::path_to_utf8(shards[s].filename()));
        GgufReader reader(shards[s]);
        auto* file = reader.metadata.get();
        if (s == 0) gguf_set_kv(vocab.get(), file);
        for (auto& [name, t] : reader.tensors) {
            if (!tensors.emplace(name, std::move(t)).second) throw std::runtime_error("Duplicate GGUF tensor: " + name);
            has_ple |= name == "per_layer_token_embd.weight";
        }
    }
    if (!has_ple) throw std::invalid_argument("Strata checkpoint has no per_layer_token_embd.weight");
    write_vocabulary(vocab.get(), destination / "tokenizer.gguf");
    std::ofstream dense(destination / "dense.bin", std::ios::binary);
    std::ostringstream rows;
    uint64_t disk = 0, pool = 0;
    size_t count = 0;
    for (const auto& [name, t] : tensors) {
        if (std::regex_match(name, expert_tensor_pattern) || name == "per_layer_token_embd.weight") continue;
        if (t.shape[2] != 1 || t.shape[3] != 1) throw std::invalid_argument("Unsupported Strata dense tensor shape: " + name);
        const auto form = required_form(name, t.type);
        if (!is_float(t.type) && form == t.type) {
            rows << name << " 0 0 0 0 " << pool << " 0 " << t.shape[0] << " " << t.shape[1]
                 << " 8 0 32 0 0 0 0 0 0 0\n";
        } else {
            if (!is_float(t.type) && !compat_bf16) {
                throw std::invalid_argument("Strata checkpoint requires compat-BF16 dequantization: " + name);
            }
            const uint64_t dst = static_cast<uint64_t>(t.shape[0]) * t.shape[1] * ggml_type_size(form);
            const uint64_t stored = t.type == GGML_TYPE_F32 && form == GGML_TYPE_F16 ? t.size : dst;
            const int kind = t.type == GGML_TYPE_F32 && form == GGML_TYPE_F16 ? 3 :
                             form == GGML_TYPE_BF16 ? 4 : form == GGML_TYPE_F16 ? 5 : 2;
            rows << name << " 0 " << kind << " " << disk << " " << stored << " " << pool << " " << dst
                 << " " << t.shape[0] << " " << (t.shape[1] == 1 ? 0 : t.shape[1])
                 << " 0 0 1 0 0 0 0 0 0 0\n";
            copy_tensor(t, form, dense, progress, name, compat_bf16);
            const std::string padding(static_cast<size_t>(align(stored) - stored), '\0');
            dense.write(padding.data(), static_cast<std::streamsize>(padding.size()));
            disk += align(stored);
            pool += align(dst);
        }
        ++count;
    }
    dense.close();
    if (!dense) throw std::runtime_error("Could not finish Strata dense weights");
    std::ofstream index(destination / "index.txt");
    index << "# strata pack index v3\n# align 64 pool " << pool << " tensors " << count << "\n" << rows.str();
    index.close();
    if (!index) throw std::runtime_error("Could not write Strata tensor index");
    std::map<int, std::map<std::string, const Tensor*>> layers;
    for (const auto& [name, t] : tensors) {
        std::smatch m;
        if (std::regex_match(name, m, expert_tensor_pattern)) layers[std::stoi(m[1])][m[2]] = &t;
    }
    if (layers.empty()) throw std::invalid_argument("Strata checkpoint has no MoE experts");
    const auto hadamard = hadamard_text(vocab.get(), tensors, layers.size());
    std::ofstream experts(destination / "native_experts.txt");
    const auto& geometry = *layers.begin()->second.at("gate");
    experts << "# strata native experts v" << (hadamard.empty() ? 4 : 5)
            << ": layer gu_type d_type offset blob_bytes gate_off up_off down_off [shard | gate,up,down]"
            << " (n_expert " << geometry.shape[2] << ")\n";
    uint64_t expert_offset = 0;
    int next_layer = 0;
    for (const auto& [layer, roles] : layers) {
        if (layer != next_layer++ || roles.size() != 3) throw std::invalid_argument("Incomplete Strata expert layer");
        const auto& gate = *roles.at("gate");
        const auto& up = *roles.at("up");
        const auto& down = *roles.at("down");
        const auto n = gate.shape[2];
        const auto router = tensors.find("blk." + std::to_string(layer) + ".ffn_gate_inp.weight");
        if (router == tensors.end() || router->second.shape[0] != gate.shape[0] || router->second.shape[1] != n) {
            throw std::invalid_argument("Strata router and routed expert counts disagree");
        }
        if (n <= 0 || n != geometry.shape[2] || gate.shape[0] != geometry.shape[0] ||
            gate.shape[1] != geometry.shape[1] || gate.shape != up.shape ||
            down.shape[0] != gate.shape[1] || down.shape[1] != gate.shape[0] ||
            down.shape[2] != n || gate.shape[3] != 1 || down.shape[3] != 1 ||
            gate.type != up.type || gate.size != up.size || gate.size % n || down.size % n) {
            throw std::invalid_argument("Inconsistent Strata expert shapes");
        }
        const auto blob = (gate.size + up.size + down.size) / n;
        const auto filename = [&](const Tensor& t) {
            // Metal can repack --native; these offsets must still address the original shards.
            return utils::path_to_utf8(t.shard.filename());
        };
        experts << layer << " " << gate.type << " " << down.type << " " << expert_offset << " " << blob
                << " " << gate.offset << " " << up.offset << " " << down.offset
                << " " << filename(gate) << "," << filename(up) << "," << filename(down) << "\n";
        expert_offset += blob * n;
    }
    experts.close();
    if (!experts) throw std::runtime_error("Could not write Strata expert layout");
    if (!hadamard.empty()) {
        std::ofstream rotation(destination / "hadamard.txt");
        rotation << hadamard;
        rotation.close();
        if (!rotation) throw std::runtime_error("Could not write Strata Hadamard layout");
    } else fs::remove(destination / "hadamard.txt");
    report(progress, "complete");
    finish_preparation(shards, destination, "gguf-pack");
}

void prepare_vocabulary(const fs::path& source, const fs::path& destination) {
    GgufReader reader(source, true);
    write_vocabulary(reader.metadata.get(), destination);
}

void finish_preparation(const std::vector<fs::path>& sources, const fs::path& destination, const std::string& mode) {
    nlohmann::json manifest = {{"schema", 1}, {"mode", mode}, {"shards", nlohmann::json::array()},
        {"pack_files", nlohmann::json::object()}};
    for (const auto& source : sources) {
        manifest["shards"].push_back({{"path", utils::path_to_utf8(fs::absolute(source))}, {"size", fs::file_size(source)}});
    }
    std::vector<std::string> outputs = {"tokenizer.gguf"};
    if (mode != "native-gguf") {
        outputs.insert(outputs.end(), {"dense.bin", "index.txt", "native_experts.txt"});
        if (fs::is_regular_file(destination / "hadamard.txt")) outputs.push_back("hadamard.txt");
        if (mode == "ready-pack") outputs.push_back("experts.bin");
    }
    for (const auto& name : outputs) manifest["pack_files"][name] = fs::file_size(destination / name);
    const auto temporary = destination / "lemonade-native.json.tmp";
    const auto final = destination / "lemonade-native.json";
    std::ofstream marker(temporary, std::ios::binary | std::ios::trunc);
    marker << manifest.dump(2) << "\n";
    marker.close();
    if (!marker) throw std::runtime_error("Could not write Strata pack completion marker");
    std::error_code error;
    fs::remove(final, error);
    if (error) throw std::runtime_error("Could not replace Strata pack completion marker: " + error.message());
    fs::rename(temporary, final);
}

std::pair<fs::path, fs::path> prepare_metal_native(const std::vector<fs::path>& shards, const fs::path& destination) {
    if (shards.empty()) throw std::invalid_argument("Strata Metal requires GGUF shards");
    GgufReader first(shards.front());
    auto tensors = first.tensors;
    fs::path ple;
    bool repack = false;
    nlohmann::json sources = nlohmann::json::array();
    for (const auto& shard : shards) {
        GgufReader file(shard);
        sources.push_back({{"path", utils::path_to_utf8(fs::absolute(shard))}, {"size", fs::file_size(shard)}});
        for (const auto& [name, tensor] : file.tensors) {
            if (name == "per_layer_token_embd.weight") ple = shard;
            else if (shard != shards.front()) {
                repack = true;
                if (!tensors.emplace(name, tensor).second) throw std::invalid_argument("Duplicate Strata Metal tensor: " + name);
            }
        }
    }
    if (ple.empty()) throw std::invalid_argument("Strata Metal checkpoint has no PLE table");
    if (!repack) return {shards.front(), ple};
    const auto receipt = destination.parent_path() / "metal.json";
    if (fs::is_regular_file(destination) && fs::is_regular_file(receipt)) {
        const auto previous = utils::JsonUtils::load_from_file(utils::path_to_utf8(receipt));
        if (previous.at("sources") == sources && previous.at("size") == fs::file_size(destination)) return {destination, ple};
    }
    fs::remove(receipt);
    tensors.erase("per_layer_token_embd.weight");
    std::vector<std::pair<std::string, const Tensor*>> ordered;
    std::map<int, std::map<std::string, const Tensor*>> experts;
    for (const auto& [name, tensor] : tensors) {
        std::smatch match;
        if (std::regex_match(name, match, expert_tensor_pattern)) experts[std::stoi(match[1])][match[2]] = &tensor;
        else ordered.emplace_back(name, &tensor);
    }
    for (const auto& [layer, roles] : experts) {
        for (const auto& role : {"gate", "up", "down"}) {
            ordered.emplace_back("blk." + std::to_string(layer) + ".ffn_" + role + "_exps.weight", roles.at(role));
        }
    }
    std::ostringstream header(std::ios::out | std::ios::binary);
    const auto number = [&](auto value) { header.write(reinterpret_cast<const char*>(&value), sizeof(value)); };
    const auto string = [&](const std::string& value) {
        number(static_cast<uint64_t>(value.size()));
        header.write(value.data(), static_cast<std::streamsize>(value.size()));
    };
    std::vector<std::pair<uint64_t, uint64_t>> kept;
    for (const auto& [key, span] : first.entries) {
        if (key.rfind("split.", 0) != 0 && key != "general.alignment") kept.push_back(span);
    }
    number(uint32_t{0x46554747}); number(uint32_t{3});
    number(static_cast<uint64_t>(ordered.size())); number(static_cast<uint64_t>(kept.size() + 1));
    for (const auto& [start, length] : kept) {
        header.write(first.raw_metadata.data() + start, static_cast<std::streamsize>(length));
    }
    string("general.alignment"); number(uint32_t{GGUF_TYPE_UINT32}); number(uint32_t{16384});
    uint64_t offset = 0;
    for (const auto& [name, tensor] : ordered) {
        string(name);
        number(tensor->dimensions);
        for (uint32_t d = 0; d < tensor->dimensions; ++d) number(static_cast<uint64_t>(tensor->shape[d]));
        number(static_cast<uint32_t>(tensor->type)); number(offset);
        offset = aligned(offset + tensor->size, 16384);
    }
    const auto metadata = header.str();
    const auto data = aligned(metadata.size(), 16384);
    const auto temporary = fs::path(destination).concat(".part");
    fs::remove(temporary);
    fs::remove(destination);
    if (fs::space(destination.parent_path()).available < data + offset) {
        throw std::runtime_error("Insufficient disk space for Strata Metal's single-file GGUF");
    }
    std::ofstream output(temporary, std::ios::binary);
    const auto sync = [&] {
        output.flush();
        if (!output) throw std::runtime_error("Could not write Strata Metal GGUF");
#ifdef __APPLE__
        // Bound dirty pages during multi-hundred-GB repacks instead of exhausting unified memory.
        const auto descriptor = ::open(temporary.c_str(), O_WRONLY);
        if (descriptor < 0) throw std::runtime_error("Could not open Strata Metal GGUF for synchronization");
        const auto result = ::fsync(descriptor);
        const auto closed = ::close(descriptor);
        if (result != 0 || closed != 0) throw std::runtime_error("Could not synchronize Strata Metal GGUF");
#endif
    };
    output.write(metadata.data(), static_cast<std::streamsize>(metadata.size()));
    const auto pad = [&](uint64_t length) {
        const std::string zeros(static_cast<size_t>(length), '\0');
        output.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    };
    pad(data - metadata.size());
    uint64_t last_sync = 0;
    const auto flush_progress = [&](const DownloadProgress&) {
        const auto position = output.tellp();
        if (position < 0) throw std::runtime_error("Could not write Strata Metal GGUF");
        if (static_cast<uint64_t>(position) - last_sync >= 1024ULL * 1024 * 1024) {
            sync();
            last_sync = static_cast<uint64_t>(position);
        }
        return true;
    };
    for (const auto& [name, tensor] : ordered) {
        copy_tensor(*tensor, tensor->type, output, flush_progress, name);
        pad(aligned(tensor->size, 16384) - tensor->size);
    }
    sync();
    output.close();
    if (!output) throw std::runtime_error("Could not finish Strata Metal GGUF");
    fs::rename(temporary, destination);
    utils::JsonUtils::save_to_file({{"sources", sources}, {"size", fs::file_size(destination)}}, utils::path_to_utf8(receipt));
    return {destination, ple};
}

bool pack_present(const fs::path& directory) {
    const auto marker = directory / "lemonade-native.json";
    if (!fs::is_regular_file(marker)) return false;
    const auto manifest = utils::JsonUtils::load_from_file(utils::path_to_utf8(marker));
    if (manifest.at("schema") != 1) return false;
    for (const auto& s : manifest.at("shards")) {
        const auto p = utils::path_from_utf8(s.at("path").get<std::string>());
        const auto relative = p.lexically_relative(fs::absolute(directory.parent_path()));
        if (relative.empty() || relative.is_absolute() ||
            std::find(relative.begin(), relative.end(), fs::path("..")) != relative.end() ||
            !fs::is_regular_file(p) || fs::file_size(p) != s.at("size").get<uint64_t>()) return false;
    }
    for (const auto& [name, size] : manifest.at("pack_files").items()) {
        if (utils::path_from_utf8(name).filename() != utils::path_from_utf8(name)) return false;
        const auto path = directory / name;
        if (!fs::is_regular_file(path) || fs::file_size(path) != size.get<uint64_t>()) return false;
    }
    return !manifest.at("shards").empty() && manifest.at("pack_files").contains("tokenizer.gguf");
}

}

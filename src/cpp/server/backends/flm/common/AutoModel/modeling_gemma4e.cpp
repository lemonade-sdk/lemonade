/// \file modeling_gemma4e.cpp
/// \brief Gemma4e class
/// \author FastFlowLM Team
/// \date 2025-09-01
/// \version 0.9.24
/// \note This is a source file for the Gemma4e class


#include "AutoModel/modeling_gemma4e.hpp"
#include "metrices.hpp"

namespace {
std::string trim_gemma4e_tool_value(std::string value) {
    size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }

    size_t end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

// --- Gemma4e tool-args parser -------------------------------------------------
// The model emits relaxed JSON for tool arguments:
//   - keys are bare identifiers (e.g. `name:`) — sometimes also "..." quoted
//   - string values are delimited by <|"|>...<|"|>, but the model frequently
//     omits the opener and/or replaces the closer with a plain `"`
//   - values can also be objects {..}, arrays [..], booleans, null, or numbers
//   - inside a string value, raw `"`, raw newlines, and even the literal
//     substring `<|"|>` can appear as content
//
// We rewrite the input into well-formed JSON via recursive-descent.
struct Gemma4eArgsParser {
    const std::string& s;
    size_t i = 0;
    static constexpr size_t marker_len = 5;
    static constexpr const char* quote_marker_lit = "<|\"|>";

    explicit Gemma4eArgsParser(const std::string& src) : s(src) {}

    void skip_ws() {
        while (i < s.size() &&
               std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    }
    void skip_ws_at(size_t& pos) const {
        while (pos < s.size() &&
               std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
    }
    bool match_marker(size_t pos) const {
        return pos + marker_len <= s.size() &&
               s.compare(pos, marker_len, quote_marker_lit) == 0;
    }

    static void json_escape_char(std::string& out, char c) {
        switch (c) {
            case '"':  out.append("\\\""); break;
            case '\\': out.append("\\\\"); break;
            case '\n': out.append("\\n");  break;
            case '\r': out.append("\\r");  break;
            case '\t': out.append("\\t");  break;
            case '\b': out.append("\\b");  break;
            case '\f': out.append("\\f");  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned char>(c));
                    out.append(buf);
                } else {
                    out.push_back(c);
                }
                break;
        }
    }

    bool match_word(const char* w, size_t len) const {
        if (i + len > s.size()) return false;
        if (s.compare(i, len, w) != 0) return false;
        if (i + len == s.size()) return true;
        unsigned char nc = static_cast<unsigned char>(s[i + len]);
        return !(std::isalnum(nc) || nc == '_');
    }

    // Parse one JSON value. `terminators` is the set of chars that end a
    // bare (undelimited) string at depth 0 (e.g. ",}" inside an object,
    // ",]" inside an array, "" at the very top).
    std::string parse_value(const std::string& terminators) {
        skip_ws();
        if (i >= s.size()) return "null";
        char c = s[i];
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (match_word("true",  4)) { i += 4; return "true";  }
        if (match_word("false", 5)) { i += 5; return "false"; }
        if (match_word("null",  4)) { i += 4; return "null";  }
        if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
        return parse_string(terminators);
    }

    std::string parse_number() {
        size_t start = i;
        if (s[i] == '-') ++i;
        while (i < s.size()) {
            char c = s[i];
            if ((c >= '0' && c <= '9') || c == '.' ||
                c == 'e' || c == 'E' || c == '+' || c == '-') ++i;
            else break;
        }
        return s.substr(start, i - start);
    }

    std::string parse_key() {
        skip_ws();
        std::string key;
        if (i >= s.size()) return key;
        if (match_marker(i)) {
            i += marker_len;
            while (i < s.size() && !match_marker(i)) key.push_back(s[i++]);
            if (match_marker(i)) i += marker_len;
        } else if (s[i] == '"') {
            ++i;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    key.push_back(s[i]);
                    key.push_back(s[i + 1]);
                    i += 2;
                } else {
                    key.push_back(s[i++]);
                }
            }
            if (i < s.size() && s[i] == '"') ++i;
        } else {
            while (i < s.size() &&
                   (std::isalnum(static_cast<unsigned char>(s[i])) ||
                    s[i] == '_')) {
                key.push_back(s[i++]);
            }
        }
        return key;
    }

    std::string parse_object() {
        // assumes s[i] == '{'
        ++i;
        std::string out = "{";
        bool first = true;
        while (i < s.size()) {
            skip_ws();
            if (i >= s.size()) break;
            if (s[i] == '}') { ++i; break; }

            std::string key = parse_key();
            if (key.empty()) {
                // can't make progress; bail
                if (i < s.size() && s[i] == '}') { ++i; }
                break;
            }
            skip_ws();
            if (i < s.size() && s[i] == ':') ++i;

            std::string val = parse_value(",}");

            if (!first) out.push_back(',');
            first = false;
            out.push_back('"');
            for (char kc : key) json_escape_char(out, kc);
            out.append("\":");
            out.append(val);

            skip_ws();
            if (i < s.size() && s[i] == ',') ++i;
        }
        out.push_back('}');
        return out;
    }

    std::string parse_array() {
        // assumes s[i] == '['
        ++i;
        std::string out = "[";
        bool first = true;
        while (i < s.size()) {
            skip_ws();
            if (i >= s.size()) break;
            if (s[i] == ']') { ++i; break; }

            std::string val = parse_value(",]");
            if (!first) out.push_back(',');
            first = false;
            out.append(val);

            skip_ws();
            if (i < s.size() && s[i] == ',') ++i;
        }
        out.push_back(']');
        return out;
    }

    // Parse a string value. Accepts three forms:
    //   - <|"|>...<|"|>  (or <|"|>...")  -- marker opener, marker or " closer
    //   - "..."                          -- regular JSON string
    //   - bare text                      -- ends at depth-0 terminator
    std::string parse_string(const std::string& terminators) {
        enum Mode { MARKER, QUOTE, BARE };
        Mode mode = BARE;
        if (match_marker(i)) { mode = MARKER; i += marker_len; }
        else if (s[i] == '"') { mode = QUOTE; ++i; }

        auto is_terminator = [&](size_t pos) {
            size_t k = pos;
            skip_ws_at(k);
            if (k >= s.size()) return true;
            return terminators.find(s[k]) != std::string::npos;
        };

        // QUOTE-mode only: honor JSON-style backslash escapes verbatim.
        auto consume_quote_escape = [&](std::string& out) {
            // s[i] == '\\'
            if (i + 1 >= s.size()) {
                json_escape_char(out, s[i]);
                ++i;
                return;
            }
            char nc = s[i + 1];
            switch (nc) {
                case '"': case '\\': case '/':
                case 'b': case 'f': case 'n': case 'r': case 't':
                    out.push_back('\\');
                    out.push_back(nc);
                    i += 2;
                    return;
                case 'u': {
                    if (i + 5 < s.size() &&
                        std::isxdigit(static_cast<unsigned char>(s[i + 2])) &&
                        std::isxdigit(static_cast<unsigned char>(s[i + 3])) &&
                        std::isxdigit(static_cast<unsigned char>(s[i + 4])) &&
                        std::isxdigit(static_cast<unsigned char>(s[i + 5]))) {
                        out.append(s, i, 6);
                        i += 6;
                        return;
                    }
                    break;
                }
                default: break;
            }
            json_escape_char(out, s[i]);
            ++i;
        };

        // For MARKER and BARE modes we first collect the raw content, then
        // JSON-encode it. Backslash policy is decided per-string:
        //   - if every `\` is followed by a valid JSON escape char, treat
        //     them as escapes (so e.g. `\n` becomes a real newline)
        //   - otherwise treat every `\` as a literal char (so Windows paths
        //     like `C:\Users\nock9\Desktop\abc.txt` are preserved verbatim)
        auto is_escape_char = [](char c) {
            return c == '"' || c == '\\' || c == '/' ||
                   c == 'b' || c == 'f' || c == 'n' ||
                   c == 'r' || c == 't' || c == 'u';
        };
        auto encode_raw = [&](const std::string& raw, std::string& out) {
            bool all_valid = true;
            for (size_t k = 0; k < raw.size(); ++k) {
                if (raw[k] == '\\') {
                    if (k + 1 >= raw.size() || !is_escape_char(raw[k + 1])) {
                        all_valid = false;
                        break;
                    }
                    ++k;
                }
            }
            for (size_t k = 0; k < raw.size(); ++k) {
                char c = raw[k];
                if (c == '\\' && all_valid && k + 1 < raw.size()) {
                    char nc = raw[k + 1];
                    if (nc == 'u' && k + 5 < raw.size() &&
                        std::isxdigit(static_cast<unsigned char>(raw[k + 2])) &&
                        std::isxdigit(static_cast<unsigned char>(raw[k + 3])) &&
                        std::isxdigit(static_cast<unsigned char>(raw[k + 4])) &&
                        std::isxdigit(static_cast<unsigned char>(raw[k + 5]))) {
                        out.append(raw, k, 6);
                        k += 5;
                    } else {
                        out.push_back('\\');
                        out.push_back(nc);
                        ++k;
                    }
                } else {
                    json_escape_char(out, c);
                }
            }
        };

        std::string value;
        std::string raw;       // used in MARKER / BARE modes
        int depth = 0;
        while (i < s.size()) {
            if (mode == MARKER) {
                if (match_marker(i)) {
                    size_t after = i + marker_len;
                    if (is_terminator(after)) { i = after; break; }
                    raw.append(quote_marker_lit, marker_len);
                    i = after;
                    continue;
                }
                if (s[i] == '"' || s[i] == '`') {
                    // The model occasionally substitutes a plain `"` or even
                    // a backtick `` ` `` for the closing <|"|> marker. Treat
                    // either as a close only when followed by a terminator,
                    // so they remain valid string content otherwise.
                    if (is_terminator(i + 1)) { ++i; break; }
                    raw.push_back(s[i]);
                    ++i;
                    continue;
                }
                raw.push_back(s[i]);
                ++i;
                continue;
            }
            if (mode == QUOTE) {
                char c = s[i];
                if (c == '\\') {
                    consume_quote_escape(value);
                    continue;
                }
                if (c == '"') { ++i; break; }
                json_escape_char(value, c);
                ++i;
                continue;
            }
            // BARE
            if (match_marker(i)) {
                size_t after = i + marker_len;
                if (depth == 0 && is_terminator(after)) { i = after; break; }
                raw.append(quote_marker_lit, marker_len);
                i = after;
                continue;
            }
            char c = s[i];
            if (depth == 0 && terminators.find(c) != std::string::npos) break;
            if (c == '{' || c == '[' || c == '(') ++depth;
            else if ((c == '}' || c == ']' || c == ')') && depth > 0) --depth;
            raw.push_back(c);
            ++i;
        }

        if (mode != QUOTE) {
            encode_raw(raw, value);
        }

        std::string out = "\"";
        out.append(value);
        out.push_back('"');
        return out;
    }
};

std::pair<std::string, json> parse_gemma4e_tool_content(std::string tool_content) {
    tool_content = trim_gemma4e_tool_value(tool_content);

    const std::string prefix = "call:";
    if (tool_content.find(prefix) == 0) {
        tool_content = trim_gemma4e_tool_value(tool_content.substr(prefix.length()));
    }

    // std::cout << "[DEBUG after prefix removal]" << std::endl;
    // std::cout << tool_content << std::endl;

    // only has tool name but no args
    size_t brace_pos = tool_content.find('{');
    if (brace_pos == std::string::npos) {
        return {trim_gemma4e_tool_value(tool_content), json::object()};
    }

    std::string tool_name = trim_gemma4e_tool_value(tool_content.substr(0, brace_pos));
    std::string args_str = trim_gemma4e_tool_value(tool_content.substr(brace_pos));

    // std::cout << "[DEBUG after parsing tool name and args]" << std::endl;
    // std::cout << "Tool Name: " << tool_name << std::endl;
    // std::cout << "Args Str: " << args_str << std::endl;

    // Rewrite the relaxed args into well-formed JSON via recursive-descent.
    Gemma4eArgsParser parser(args_str);
    parser.skip_ws();
    std::string normalized;
    if (parser.i < args_str.size() && args_str[parser.i] == '{') {
        normalized = parser.parse_object();
    } else {
        // Unexpected shape; wrap whatever we get into an object so the
        // downstream JSON parse step still has a sensible structure.
        normalized = parser.parse_value("");
    }

    // std::cout << "[DEBUG normalized args]" << std::endl;
    // std::cout << normalized << std::endl;

    // Final: parse to a real JSON object; fall back to empty object on failure.
    json args_json = json::object();
    try {
        args_json = json::parse(normalized);
    } catch (const std::exception& e) {
        std::cerr << "[WARNING] Failed to parse tool args as JSON: "
                  << e.what() << std::endl;
        std::cerr << "Raw args: " << normalized << std::endl;
    }

    return {tool_name, args_json};
}
}


/************              Gemma4e family            **************/
Gemma4e::Gemma4e(xrt::device* npu_device_inst) : AutoModel(npu_device_inst, "Gemma4e") {}

void Gemma4e::load_model(std::string model_path, json model_info, int default_context_length, bool enable_preemption) {

    this->_shared_load_model(model_path, model_info, default_context_length, enable_preemption);

    this->q4nx = std::make_unique<Q4NX>(this->model_path);
    this->lm_engine = std::make_unique<gemma4e_npu>(*this->lm_config, this->npu.get(), this->MAX_L);

    this->lm_engine->load_weights(*this->q4nx);
    //free the q4nx
    this->q4nx.reset();
    this->lm_engine->clear_context();
    this->setup_tokenizer(model_path);
    this->sampler.reset();

    this->enable_tool = (model_info["size"] > 800000000)? true : false;

    sampler_config config;
    config.top_k = 64;
    config.top_p = 0.95;
    config.min_p = 0.0;
    config.temperature = 1.0;
    config.rep_penalty = 1.0;
    config.freq_penalty = 1.0;
    config.pre_penalty = 1.0f;

    this->set_sampler(config);
    for (size_t i = 0; i < PROFILER_TYPE_NUM; i++) {
        this->profiler_list[i].reset();
    }
}

void Gemma4e::setup_tokenizer(std::string model_path) {
    auto tokenizer_config = this->_shared_setup_tokenizer(model_path);
}

std::string Gemma4e::apply_chat_template(nlohmann::ordered_json& messages, nlohmann::ordered_json tools) {
    minja::chat_template_inputs inputs;
    inputs.add_generation_prompt = true;
    inputs.messages = messages;
    inputs.extra_context = this->extra_context;
    inputs.extra_context["enable_thinking"] = this->enable_think;
    if (!tools.empty())
        inputs.tools = tools;
    return this->chat_tmpl->apply(inputs);
}

bool Gemma4e::insert(chat_meta_info_t& meta_info, lm_uniform_input_t& input, std::function<bool()> is_cancelled) {
    this->profiler_list[TKOEN_ENCODE_TIME].start();
    std::string templated_text;

    if (input.messages.empty() && input.prompt.empty()) {
        header_print("WARNING", "No messages or prompt provided");
        return false;
    }

    constexpr bool DEBUG_IMAGE_PREPROCESS = false;
    gemma4e_image_payload_t image_payload;
    gemma4e_audio_payload_t audio_payload;
    audio_payload.num_audios = 0;
    image_payload.num_images = 0;

    float max_support_audio_length_seconds = 30.0f;
    int total_audio_clips = 0;
    std::vector<audio_data_t> audio_data_list;

    if (!input.messages.empty()) { // Server Processing
        int total_images = 0;
        for (auto& message : input.messages) {
            // Process Images
            if (message.contains("images")) {
                for (auto& img : message["images"]) {
                    std::string img_str = img.get<std::string>();
                    if (!img_str.empty()) total_images++;

                    gemma4e_image_t image = this->load_image_base64(img_str);
                    std::vector<bf16> pixel_values;
                    std::pair<int, int> patch_element_per_patch;
                    uint32_t valid_patch_size = 0;
                    uint32_t num_soft_tokens = 0;
                    std::vector<int> image_grid_pairs;

                    preprocess_image(image, patch_element_per_patch, valid_patch_size, pixel_values, image_grid_pairs, num_soft_tokens);

                    image_payload.image_patch__element_per_patch.push_back(patch_element_per_patch);
                    image_payload.valid_patch_size_per_image.push_back(valid_patch_size);
                    image_payload.pixel_values.push_back(pixel_values);
                    image_payload.image_grid_pairs_per_image.push_back(image_grid_pairs);
                    image_payload.num_soft_tokens_per_image.push_back(num_soft_tokens);
                    image_payload.num_images++;
                }
            }
            // Process Audios
            if (message.contains("audios")) {
                gemma4e_npu *gemma4e_engine = dynamic_cast<gemma4e_npu*>(this->lm_engine.get());
                for (auto& aud : message["audios"]) {
                    std::string audio_str = aud.get<std::string>();
                    audio_data_t audio_data = this->load_audio_base64(audio_str, gemma4e_engine->Gemma4E_Audio_resample_rate, MonoDownmixMode::MEAN);
                    if (audio_data.channels > 1) {
                        std::cerr << "only mono audio is supported." << std::endl;
                        exit(-1);
                    }
                    std::vector<audio_data_t> clipped_audio_data = this->clip_audio_length(audio_data, max_support_audio_length_seconds);
                    audio_data_list.insert(audio_data_list.end(), clipped_audio_data.begin(), clipped_audio_data.end());
                    total_audio_clips += clipped_audio_data.size();
                    if (clipped_audio_data.size() > 1) {
                        header_print_g("FLM", "Audio in message is split into " + std::to_string(clipped_audio_data.size()) + " chunks for processing.");
                        std::cout << std::endl;
                    }
                }
            }
        }
        header_print("FLM", "Total images: " << total_images);
    }
    else { // CLI Processing
        if (input.audios.size() > 0) {
            gemma4e_npu *gemma4e_engine = dynamic_cast<gemma4e_npu*>(this->lm_engine.get());
            for (int i = 0; i < input.audios.size(); i++) {
                std::string audio_str = input.audios[i];
                audio_data_t audio_data = this->load_audio(audio_str, gemma4e_engine->Gemma4E_Audio_resample_rate, MonoDownmixMode::MEAN);

                if (audio_data.channels > 1) {
                    std::cerr << "only mono audio is supported, but got " << audio_data.original_channels << " channels. Please convert it to mono first." << std::endl;
                    exit(-1);
                }

                // apply clipping
                std::vector<audio_data_t> clipped_audio_data = this->clip_audio_length(audio_data, max_support_audio_length_seconds);
                audio_data_list.insert(audio_data_list.end(), clipped_audio_data.begin(), clipped_audio_data.end());
                total_audio_clips += clipped_audio_data.size();

                if (clipped_audio_data.size() > 1) {
                    header_print_g("FLM", "Audio[" + std::to_string(i) + "] is split into " + std::to_string(clipped_audio_data.size()) + " chunks for processing.");
                    std::cout << std::endl;
                }
            }
        }

        if (input.images.size() > 0) {
            for (const auto& img_str : input.images) {
                gemma4e_image_t image = this->load_image(img_str);
                std::vector<bf16> pixel_values;
                std::pair<int, int> patch_element_per_patch;
                uint32_t valid_patch_size = 0;
                uint32_t num_soft_tokens = 0;
                std::vector<int> image_grid_pairs;

                preprocess_image(image, patch_element_per_patch, valid_patch_size, pixel_values, image_grid_pairs, num_soft_tokens);

                image_payload.image_patch__element_per_patch.push_back(patch_element_per_patch);
                image_payload.valid_patch_size_per_image.push_back(valid_patch_size);
                image_payload.pixel_values.push_back(pixel_values);
                image_payload.image_grid_pairs_per_image.push_back(image_grid_pairs);
                image_payload.num_soft_tokens_per_image.push_back(num_soft_tokens);
                image_payload.num_images++;
            }
        }
    }

    if (!audio_data_list.empty()) {
        this->extract_spectrogram(audio_data_list, audio_payload);

        gemma4e_npu *gemma4e_engine = dynamic_cast<gemma4e_npu*>(this->lm_engine.get());
        const unsigned int conv2d_kernel = gemma4e_engine->Gemma4E_Audio_conv2d_kernel_size;
        const unsigned int conv2d_stride = gemma4e_engine->Gemma4E_Audio_conv2d_Stride;
        const unsigned int conv2d_padding = gemma4e_engine->Gemma4e_Audio_conv2d_Padding;
        const unsigned int max_audio_seq_length = max_support_audio_length_seconds * gemma4e_engine->Gemma4E_Audio_resample_rate;

        constexpr float frame_length_ms = 20.0f;
        constexpr float hop_length_ms   = 10.0f;

        for (int i = 0; i < audio_payload.num_audios; i++) {
            const int num_samples = static_cast<int>(audio_data_list[i].num_samples);
            const int sampling_rate = audio_data_list[i].sample_rate;

            const int frame_length = static_cast<int>(std::round(sampling_rate * frame_length_ms / 1000.0f));
            const int hop_length   = static_cast<int>(std::round(sampling_rate * hop_length_ms / 1000.0f));
            const int frame_size_for_unfold = frame_length + 1;

            const int pad_left = frame_length / 2;
            const int padded_samples = num_samples + pad_left;
            int num_mel_frames = (padded_samples - frame_size_for_unfold) / hop_length + 1;

            unsigned int num_tokens = 0;
            if (num_mel_frames > 0) {
                int t = num_mel_frames;
                for (int layer = 0; layer < 2; layer++) {
                    int t_padded = t + 2 * static_cast<int>(conv2d_padding);
                    t = (t_padded - static_cast<int>(conv2d_kernel)) / static_cast<int>(conv2d_stride) + 1;
                }
                assert(t < max_audio_seq_length);
                num_tokens = t;
            }
            audio_payload.num_soft_tokens_per_audio.push_back(num_tokens);
        }
    }
    if (!input.messages.empty()) { // already a formated messages, usually from REST API
        nlohmann::ordered_json gemma4_message = nlohmann::ordered_json::array();
        for (const auto& item : input.messages) {
            if (!item.contains("images") && !item.contains("audios")) {
                gemma4_message.push_back(item);
                continue;
            }

            nlohmann::ordered_json newContent = nlohmann::ordered_json::array();
            if (item.contains("images")) {
                for (const auto& img : item["images"]) {
                    newContent.push_back({{"type", "image"}, {"image", img}});
                }
            }
            if (item.contains("audios")) {
                for (const auto& aud : item["audios"]) {
                    newContent.push_back({{"type", "audio"}, {"audio", aud}});
                }
            }
            newContent.push_back({{"type", "text"}, {"text", item.value("content", "")}});

            nlohmann::ordered_json newItem = {
                {"role", item.value("role", "user")},
                {"content", newContent}
            };
            gemma4_message.push_back(newItem);
        }
        templated_text = this->apply_chat_template(gemma4_message, input.tools);
    }
    else if (!input.prompt.empty()) { // a pure text, usually from the cli
        nlohmann::ordered_json messages;
        nlohmann::ordered_json content;
        content["role"] = "user";
        content["content"] = nlohmann::ordered_json::array();

        for (int i = 0; i < input.images.size(); i++) {
            content["content"].push_back({{"type", "image"}, {"image", input.images[i]}});
        }
        for (int i = 0; i < total_audio_clips; i++) {
            content["content"].push_back({{"type", "audio"}, {"audio", input.audios[0]}}); // placeholder
        }

        content["content"].push_back({{"type", "text"}, {"text", input.prompt}});
        messages.push_back(content);
        templated_text = this->apply_chat_template(messages);
    }

    std::vector<int> tokens_init = this->tokenizer->encode(templated_text);

    // update the tokens to include the image tokens
    std::vector<int> tokens;

    int total_image_tokens = 0;
    for (int i = 0; i < image_payload.num_images; i++) {
        total_image_tokens += image_payload.num_soft_tokens_per_image[i];
    }

    int total_audio_tokens = 0;
    for (int i = 0; i < audio_payload.num_audios; i++) {
        total_audio_tokens += audio_payload.num_soft_tokens_per_audio[i];
    }

    tokens.reserve(tokens_init.size() + total_image_tokens + total_audio_tokens);

    int image_counter = 0;
    int audio_counter = 0;

    for (int i = 0; i < tokens_init.size(); i++) {
        if (tokens_init[i] == image_token_id) {
            tokens.push_back(boi_token_id); // the first image soft token id, which is reserved for the model to identify the image position, the rest of the soft tokens for this image will be continuous following this id
            for (int j = 0; j < image_payload.num_soft_tokens_per_image[image_counter]; j++) {
                tokens.push_back(image_token_id);
            }
            tokens.push_back(eoi_token_id); // a separator token between images, not necessary but can help the model to better distinguish different images
            image_counter++;
        }
        else if (tokens_init[i] == audio_token_id){
            tokens.push_back(boa_token_id); // the first audio soft token id, which is reserved for the model to identify the audio position, the rest of the soft tokens for this audio will be continuous following this id
            for (int j = 0; j < audio_payload.num_soft_tokens_per_audio[audio_counter]; j++) {
                tokens.push_back(audio_token_id);
            }
            tokens.push_back(eoa_token_id); // a separator token between audios, not necessary but can help the model to better distinguish different audios
            audio_counter++;
        }
        else {
            tokens.push_back(tokens_init[i]);
        }
    }
    assert(image_counter == image_payload.num_images);
    assert(audio_counter == audio_payload.num_audios);

    this->profiler_list[TKOEN_ENCODE_TIME].stop(tokens.size());

    // ----------------------------------------------------------------------
    // Prompt-cache aware multi-modal alignment.
    //
    // AutoModel::_shared_insert prefix-matches `tokens` against `checkpoint_his`
    // over the FULL length of `checkpoint_his`. If every token matches, it
    // erases that prefix before prefilling; otherwise it calls clear_context()
    // and skips nothing. We must NOT erase `tokens` here -- _shared_insert
    // needs the untrimmed sequence to run that very check. What we DO need
    // to fix up locally is the multi-modal payload (which carries
    // pixels/spectrograms for the WHOLE prompt, including images/audios from
    // earlier turns that are already cached): drop the fully-cached leading
    // images/audios so the surviving payload aligns with the surviving soft
    // tokens after _shared_insert performs its own erase.
    // ----------------------------------------------------------------------
    size_t prefix_skip_count = 0;
    {
        const size_t idx = this->checkpoint_his.size();
        for (size_t i = 0; i < idx; i++) {
            if (i < tokens.size() && tokens[i] == this->checkpoint_his[i]) {
                prefix_skip_count++;
            } else {
                break;
            }
        }
        // Must match the entirety of checkpoint_his, otherwise _shared_insert
        // will clear the context and not skip anything.
        if (prefix_skip_count != idx) {
            prefix_skip_count = 0;
        }

        if (prefix_skip_count > 0 && (image_payload.num_images > 0 || audio_payload.num_audios > 0)) {
            // Count modal soft-tokens that landed inside the cached prefix.
            int skipped_image_tokens = 0;
            int skipped_audio_tokens = 0;
            for (size_t i = 0; i < prefix_skip_count; i++) {
                if (tokens[i] == image_token_id) skipped_image_tokens++;
                else if (tokens[i] == audio_token_id) skipped_audio_tokens++;
            }

            // Walk images in order; each image's soft-token block is either
            // entirely inside the cached prefix or not at all (chat-template
            // boundaries never split a single image's block).
            size_t images_to_drop = 0;
            {
                int consumed = 0;
                for (unsigned i = 0; i < image_payload.num_images; i++) {
                    const int n = static_cast<int>(image_payload.num_soft_tokens_per_image[i]);
                    if (consumed + n <= skipped_image_tokens) {
                        consumed += n;
                        images_to_drop++;
                    } else {
                        break;
                    }
                }
            }

            // Same for audios.
            size_t audios_to_drop = 0;
            {
                int consumed = 0;
                for (unsigned i = 0; i < audio_payload.num_audios; i++) {
                    const int n = static_cast<int>(audio_payload.num_soft_tokens_per_audio[i]);
                    if (consumed + n <= skipped_audio_tokens) {
                        consumed += n;
                        audios_to_drop++;
                    } else {
                        break;
                    }
                }
            }

            auto drop_front = [](auto& vec, size_t n) {
                if (n == 0) return;
                if (n >= vec.size()) { vec.clear(); return; }
                vec.erase(vec.begin(), vec.begin() + n);
            };

            if (images_to_drop > 0) {
                drop_front(image_payload.image_patch__element_per_patch, images_to_drop);
                drop_front(image_payload.valid_patch_size_per_image,     images_to_drop);
                drop_front(image_payload.pixel_values,                   images_to_drop);
                drop_front(image_payload.image_grid_pairs_per_image,     images_to_drop);
                drop_front(image_payload.num_soft_tokens_per_image,      images_to_drop);
                image_payload.num_images -= static_cast<unsigned>(images_to_drop);
                header_print("FLM",
                    "Prompt-cache hit: dropped " << images_to_drop
                    << " cached image(s) from payload");
            }

            if (audios_to_drop > 0) {
                drop_front(audio_payload.mel_spectrograms,                audios_to_drop);
                drop_front(audio_payload.mel_spectrogram_frames_per_audio, audios_to_drop);
                drop_front(audio_payload.mel_spectrogram_bins_per_audio,   audios_to_drop);
                drop_front(audio_payload.num_soft_tokens_per_audio,        audios_to_drop);
                audio_payload.num_audios -= static_cast<unsigned>(audios_to_drop);
                header_print("FLM",
                    "Prompt-cache hit: dropped " << audios_to_drop
                    << " cached audio(s) from payload");
            }
        }
    }

    // find the last image token index, expressed relative to the tokens that
    // will SURVIVE _shared_insert's prefix erase (i.e. shifted by -prefix_skip_count).
    int last_image_token_index = -1;
    for (int i = static_cast<int>(prefix_skip_count); i < (int)tokens.size(); i++) {
        if ((tokens[i] == image_token_id || tokens[i] == boi_token_id)) {
            last_image_token_index = i - static_cast<int>(prefix_skip_count);
        }
    }
    last_image_token_index++; // plus the end of image tokens

    // hardware
    gemma4e_multi_modal_payload_t multi_modal_payload;
    multi_modal_payload.image_payload = image_payload;
    multi_modal_payload.audio_payload = audio_payload;

    int restore_idx = -1;
    gemma4e_npu *gemma4e_engine = dynamic_cast<gemma4e_npu*>(this->lm_engine.get());
    const bool has_multimodal = image_payload.num_images > 0 || audio_payload.num_audios > 0;

    if (meta_info.restore_allowed) {
        restore_idx = gemma4e_engine->restore();
        this->total_tokens = restore_idx;
        this->token_history = checkpoint_his; // restore the token history to be consistent with the restored KV cache, which is crucial for correct functioning of _shared_insert's prefix-matching logic
    }

    bool success = has_multimodal
        ? this->_shared_insert(meta_info, tokens, is_cancelled, &multi_modal_payload, last_image_token_index)
        : this->_shared_insert(meta_info, tokens, is_cancelled, nullptr);

    checkpoint_his = token_history;
    int checkpoint_idx = gemma4e_engine->checkpoint();
    return success;
}

std::string Gemma4e::generate(chat_meta_info_t& meta_info, int length_limit, std::ostream& os, std::function<bool()> is_cancelled) {
    std::vector<int> sampled_tokens;
    std::string result;
    if (length_limit > 0){
        sampled_tokens.reserve(length_limit);
    }
    else{
        sampled_tokens.reserve(4096);
    }
    assert(this->last_token != -1);

    stop_reason_t reason = EOT_DETECTED;
    int last_sampled_token = this->last_token;

    this->token_history.push_back(last_token);

    if (this->is_normal_token(last_sampled_token) && last_sampled_token != -1){
        std::string token_str = this->tokenizer->run_time_decoder(last_sampled_token);
        result += token_str;
        os << token_str << std::flush;

    }
    if (this->is_eos(last_sampled_token)){
        return result;
    }
    this->profiler_list[DECODING_TIME].reset();
    this->profiler_list[TKOEN_DECODE_TIME].reset();
    if (this->total_tokens >= this->MAX_L){
        header_print("WARNING", "Max length reached, stopping generation...");
        reason = MAX_LENGTH_REACHED;
        return result;
    }
    while (this->total_tokens < this->MAX_L){
        if (is_cancelled()) {
            reason = CANCEL_DETECTED;
            // reset stream content
            buffer_.clear();
            current_mode_ = StreamEventType::CONTENT;
            tool_name_.clear();
            is_in_tool_block_ = false;
            break;
        }
        this->profiler_list[DECODING_TIME].start();
        buffer<bf16> y = this->lm_engine->forward(last_sampled_token);
        this->profiler_list[DECODING_TIME].stop(1);

        this->profiler_list[SAMPLING_TIME].start();
        int sampled_token = this->sampler->sample(y);
        this->profiler_list[SAMPLING_TIME].stop(1);
        this->total_tokens++;
        last_sampled_token = sampled_token;

        this->profiler_list[TKOEN_DECODE_TIME].start();
        if (this->is_normal_token(sampled_token)){ // filter out special tokens
            std::string token_str = this->tokenizer->run_time_decoder(sampled_token);
            os << token_str << std::flush;
            result += token_str;
        }
        this->profiler_list[TKOEN_DECODE_TIME].stop(1);
        this->token_history.push_back(sampled_token);
        if (this->is_eos(sampled_token)){
            meta_info.generated_tokens++;
            this->lm_engine->forward(last_sampled_token);
            break;
        }
        meta_info.generated_tokens++;
        if ((length_limit > 0) && (meta_info.generated_tokens >= length_limit)){
            reason = MAX_LENGTH_REACHED;
            break;
        }
    }
    meta_info.decoding_duration = (uint64_t)(time_utils::cast_to_us(this->profiler_list[DECODING_TIME].get_total_time()).first) * 1e3;
    meta_info.stop_reason = reason;
    if (this->total_tokens >= this->MAX_L){
        header_print("WARNING", "Max length reached, stopping generation...");
    }
    std::cout << std::endl;
    header_print("FLM", "Model RAW Output: \n" + result);

    if (!this->enable_think) {
        gemma4e_npu *gemma4e_engine = dynamic_cast<gemma4e_npu*>(this->lm_engine.get());
        int checkpoint_idx = gemma4e_engine->checkpoint();
        // copy the token history at the checkpoint except the last one token, which is the start token for generation and should not be included in the checkpoint history
        checkpoint_his = token_history;
        checkpoint_his.pop_back();
    }

    return result;
}

std::string Gemma4e::generate_with_prompt(chat_meta_info_t& meta_info, lm_uniform_input_t& input, int length_limit, std::ostream& os) {
    if (!this->insert(meta_info, input)) {
        return "";
    }
    if (this->enable_think) {
        os << "<think>\n" << std::flush;
    }

    gemma4e_npu *gemma4e_engine = dynamic_cast<gemma4e_npu*>(this->lm_engine.get());
    int checkpoint_idx = gemma4e_engine->checkpoint();
    int restore_idx = gemma4e_engine->restore();
    header_print_r("FLM", "Checkpoint before generation: " << checkpoint_idx << ", restore point: " << restore_idx << ", user context length: " << this->token_history.size());
    return this->_shared_generate(meta_info, length_limit, os);
}

// Non-stream
NonStreamResult Gemma4e::parse_nstream_content(const std::string response_text) {
    NonStreamResult result;

    std::string think_start_tag = "<|channel>thought";
    std::string think_end_tag = "<channel|>";
    std::string tool_start_tag = "<|tool_call>";
    std::string tool_end_tag = "<tool_call|>";
    std::string tool_resp_tag = "<|tool_response>";

    size_t think_start_pos = response_text.find(think_start_tag);
    size_t think_end_pos = response_text.find(think_end_tag);
    size_t tool_start_pos = response_text.find(tool_start_tag);
    size_t tool_end_pos = response_text.find(tool_end_tag, tool_start_pos == std::string::npos ? 0 : tool_start_pos + tool_start_tag.length());

    bool is_reasoning = (think_start_pos != std::string::npos && think_end_pos != std::string::npos && think_end_pos > think_start_pos);
    bool is_tool = (tool_start_pos != std::string::npos);

    // 1. Parse Reasoning Content
    if (is_reasoning) {
        size_t start = think_start_pos + think_start_tag.length();
        result.reasoning_content = response_text.substr(start, think_end_pos - start);
    }

    // 2. Parse Tool Calling
    if (is_tool) {
        size_t start = tool_start_pos + tool_start_tag.length();
        if (tool_end_pos == std::string::npos || tool_end_pos < start) {
            tool_end_pos = response_text.find(tool_resp_tag, start);
            if (tool_end_pos == std::string::npos) {
                tool_end_pos = response_text.length();
            }
        }
        std::string tool_content = response_text.substr(start, tool_end_pos - start);
        auto parsed_tool = parse_gemma4e_tool_content(tool_content);
        result.tool_name = parsed_tool.first;
        result.tool_args = parsed_tool.second.dump();
    }
    // 3. Parse Normal Content
    else {
        if (is_reasoning) {
            // Content is whatever comes AFTER the reasoning block
            result.content = response_text.substr(think_end_pos + think_end_tag.length());
        } else {
            // No reasoning, no tools -> the whole text is content
            result.content = response_text;
        }

        // Cleanup: Strip out <|tool_response> if the model accidentally hallucinated it into plain text
        size_t resp_pos = 0;
        while ((resp_pos = result.content.find(tool_resp_tag, resp_pos)) != std::string::npos) {
            result.content.erase(resp_pos, tool_resp_tag.length());
        }
    }

    return result;
}


// Stream
StreamResult Gemma4e::parse_stream_content(const std::string content) {
    return parse_stream_content_impl(content, false);
}

StreamResult Gemma4e::parse_stream_content_final(const std::string content) {
    return parse_stream_content_impl(content, true);
}

StreamResult Gemma4e::parse_stream_content_impl(const std::string content, bool is_final) {
    const std::string MARKER_THINK_START = "<|channel>thought";
    const std::string MARKER_THINK_END = "<channel|>";
    const std::string MARKER_TOOL_START = "<|tool_call>";
    const std::string MARKER_TOOL_END = "<tool_call|>";
    const std::string MARKER_TOOL_RESP = "<|tool_response>";

    StreamResult result;
    buffer_ += content;

    while (true) {
        if (is_in_tool_block_) {
            size_t tool_end_pos = buffer_.find(MARKER_TOOL_END);
            size_t tool_resp_pos = buffer_.find(MARKER_TOOL_RESP);

            if (tool_end_pos != std::string::npos || tool_resp_pos != std::string::npos || (is_final && !buffer_.empty())) {
                size_t actual_end_pos = buffer_.size();
                size_t skip_length = 0;

                if (tool_end_pos != std::string::npos) {
                    actual_end_pos = tool_end_pos;
                    skip_length = MARKER_TOOL_END.length();
                }
                if (tool_resp_pos != std::string::npos && tool_resp_pos < actual_end_pos) {
                    actual_end_pos = tool_resp_pos;
                    skip_length = MARKER_TOOL_RESP.length();
                }

                std::string tool_content = buffer_.substr(0, actual_end_pos);

                // std::cout << "DEBUG" << std::endl;
                // std::cout << tool_content << std::endl; // For debugging: see the raw tool content extracted from the stream

                buffer_ = buffer_.substr(actual_end_pos + skip_length);
                is_in_tool_block_ = false;

                result.type = StreamEventType::TOOL_DONE;

                static int tool_counter = 0;
                result.tool_id = "call_" + std::to_string(std::time(nullptr)) + "_" + std::to_string(tool_counter++);
                auto parsed_tool = parse_gemma4e_tool_content(tool_content);
                result.tool_name = parsed_tool.first;
                result.tool_args_str = parsed_tool.second.dump();
                return result;
            }
            else {
                result.type = StreamEventType::WAITING;
                return result;
            }
        }

        // Find the earliest occurring marker in the buffer to avoid skipping tags
        size_t pos_tool_start = buffer_.find(MARKER_TOOL_START);
        size_t pos_tool_resp  = buffer_.find(MARKER_TOOL_RESP);
        size_t pos_think      = std::string::npos;

        if (current_mode_ == StreamEventType::CONTENT) {
            pos_think = buffer_.find(MARKER_THINK_START);
        }
        else if (current_mode_ == StreamEventType::REASONING) {
            pos_think = buffer_.find(MARKER_THINK_END);
        }

        size_t min_pos = std::string::npos;
        if (pos_tool_start != std::string::npos) min_pos = std::min(min_pos, pos_tool_start);
        if (pos_tool_resp != std::string::npos)  min_pos = std::min(min_pos, pos_tool_resp);
        if (pos_think != std::string::npos)      min_pos = std::min(min_pos, pos_think);

        // Flush the text content before the earliest marker
        if (min_pos != std::string::npos && min_pos > 0) {
            result.content = buffer_.substr(0, min_pos);
            result.type = current_mode_;
            buffer_ = buffer_.substr(min_pos);
            return result;
        }

        // Process the exact marker located at index 0
        if (min_pos == 0) {
            if (pos_tool_resp == 0) {
                buffer_ = buffer_.substr(MARKER_TOOL_RESP.length());
                continue;
            }
            if (pos_tool_start == 0) {
                is_in_tool_block_ = true;
                buffer_ = buffer_.substr(MARKER_TOOL_START.length());
                continue;
            }
            if (pos_think == 0) {
                if (current_mode_ == StreamEventType::CONTENT) {
                    buffer_ = buffer_.substr(MARKER_THINK_START.length());
                    current_mode_ = StreamEventType::REASONING;
                } else {
                    buffer_ = buffer_.substr(MARKER_THINK_END.length());
                    current_mode_ = StreamEventType::CONTENT;
                }
                continue;
            }
        }

        // Safe Flush Mechanism
        std::vector<std::string> active_markers;
        active_markers.push_back(MARKER_TOOL_START);
        active_markers.push_back(MARKER_TOOL_RESP);

        if (current_mode_ == StreamEventType::CONTENT) {
            active_markers.push_back(MARKER_THINK_START);
        }
        else if (current_mode_ == StreamEventType::REASONING) {
            active_markers.push_back(MARKER_THINK_END);
        }

        size_t safe_flush_len = buffer_.length();
        for (const auto& marker : active_markers) {
            for (size_t i = 1; i <= marker.length() && i <= buffer_.length(); ++i) {
                if (buffer_.compare(buffer_.length() - i, i, marker, 0, i) == 0) {
                    safe_flush_len = std::min(safe_flush_len, buffer_.length() - i);
                }
            }
        }

        if (safe_flush_len > 0) {
            result.content = buffer_.substr(0, safe_flush_len);
            result.type = current_mode_;
            buffer_ = buffer_.substr(safe_flush_len);
            return result;
        }
        else if (buffer_.length() > 0) {
            result.type = StreamEventType::WAITING;
            return result;
        }

        break;
    }

    result.type = current_mode_;
    return result;
}

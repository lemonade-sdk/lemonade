/// \file automodel.cpp
/// \brief automodel class
/// \author FastFlowLM Team
/// \date 2025-09-01
/// \version 0.9.24
/// \note This is a source file for the auto_model class

#include "AutoModel/automodel.hpp"


AutoModel::AutoModel(xrt::device* npu_device_inst, std::string current_model) {
    this->npu_device_inst = npu_device_inst;
    this->current_model = current_model;
    this->total_tokens = 0;
    this->profiler_list.resize(PROFILER_TYPE_NUM);
    for (size_t i = 0; i < PROFILER_TYPE_NUM; i++) {
        this->profiler_list[i] = profiler();
    }
    this->last_prefill_time = { 0, "us" };
    this->token_history.reserve(MAX_L);
}


std::string AutoModel::get_current_model() {
    return this->current_model;
}

/// \brief Setup the tokenizer
/// \note The function will setup the tokenizer
nlohmann::json AutoModel::_shared_setup_tokenizer(std::string model_path) {
    // load tokenizer configurations
    #ifdef _WIN32
    std::string tokenizer_config_path = model_path + "\\tokenizer_config.json";
    #else
    std::string tokenizer_config_path = model_path + "/tokenizer_config.json";
    #endif
    std::ifstream fs_config(tokenizer_config_path, std::ios::in | std::ios::binary);
    if (fs_config.fail()) {
        std::cerr << "Cannot open " << tokenizer_config_path << std::endl;
        exit(1);
    }
    std::string data_config;
    fs_config.seekg(0, std::ios::end);
    size_t size_config = static_cast<size_t>(fs_config.tellg());
    fs_config.seekg(0, std::ios::beg);
    data_config.resize(size_config);
    fs_config.read(data_config.data(), size_config);
    fs_config.close();
    auto tokenizer_config = nlohmann::json::parse(data_config);
    // check if bos_token is null
    if (tokenizer_config["bos_token"].is_null()) {
        this->has_bos_token = false;
    }
    else {
        this->has_bos_token = true;
    }
    // load chat template
    //check if chat_template.jinja exists
    #ifdef _WIN32
    std::string chat_template_path = model_path + "\\chat_template.jinja";
    #else
    std::string chat_template_path = model_path + "/chat_template.jinja";
    #endif
    if (std::filesystem::exists(chat_template_path) == false) {
        // no jinja file exist, see if chat_template in tokenizer_config.json
        if (tokenizer_config.find("chat_template") == tokenizer_config.end()) {
            header_print("ERROR", "No template file found and no chat_template in tokenizer_config.json");
            exit(1);
        }
    }
    else {
        std::ifstream fs_template(chat_template_path, std::ios::in | std::ios::binary);
        if (fs_template.fail()) {
            std::cerr << "Cannot open " << chat_template_path << std::endl;
            exit(1);
        }
        std::string chat_template;
        fs_template.seekg(0, std::ios::end);
        size_t size_template = static_cast<size_t>(fs_template.tellg());
        fs_template.seekg(0, std::ios::beg);
        chat_template.resize(size_template);
        fs_template.read(chat_template.data(), size_template);
        fs_template.close();
        // overwrite chat_template in tokenizer_config.json
        tokenizer_config["chat_template"] = chat_template;
    }

    if (tokenizer_config["eos_token"].is_null()) {
        header_print("ERROR", "eos_token is null in tokenizer_config.json");
        exit(1);
    }
    this->chat_tmpl = std::make_unique<minja::chat_template>(
        tokenizer_config["chat_template"],
        this->has_bos_token ? tokenizer_config["bos_token"] : "",
        tokenizer_config["eos_token"]
    );

    if (this->has_bos_token) {
        this->bos_token_id = tokenizer_config["bos_token_id"].get<int>();
    }
    else {
        this->bos_token_id = -1;
    }
    this->eos_token = tokenizer_config["eos_token"].get<std::string>();
    for (auto& token : tokenizer_config["eos_token_id"]) {
        this->eos_token_ids.push_back(token.get<int>());
    }
    this->user_system_prompt = "";
    this->extra_context["user_system_prompt"] = this->user_system_prompt;
    return tokenizer_config;
}


void AutoModel::_shared_load_model(std::string model_path, json model_info, int default_context_length, bool enable_preemption) {
    if (this->is_model_loaded && this->model_path == model_path) {
        header_print("FLM", "Model already loaded: " << this->model_path);
        return;
    }

    this->model_path = model_path;
    header_print("FLM", "Loading model: " << this->model_path);
    this->lm_config = std::make_unique<LM_Config>();
    this->lm_config->from_pretrained(this->model_path);
    if (this->npu_device_inst == nullptr) {
        header_print("ERROR", "NPU device instance is nullptr");
        exit(1);
    }
    this->npu = std::make_unique<npu_xclbin_manager>(npu_device::device_npu2, this->npu_device_inst, enable_preemption);
    this->enable_preemption = enable_preemption;
    // Set context length: use provided value if not -1, otherwise use model default
    if (default_context_length != -1) {
        this->MAX_L = default_context_length;
    } else {
        this->MAX_L = model_info["default_context_length"];
    }

    this->is_model_loaded = true;

    this->token_history.clear();
    this->token_history.reserve(this->MAX_L);
    this->tokenizer = std::make_unique<Tokenizer>(this->model_path);

    this->last_token = -1;
    this->total_tokens = 0;
}

bool AutoModel::_shared_insert(chat_meta_info_t& meta_info, std::vector<int>& tokens, std::function<bool()> is_cancelled, void* payload, int first_len_run) {

    // print token history
    // header_print("DEBUG", "Current token history: ");
    // for (size_t i = 0; i < this->token_history.size(); i++) {
    //     std::cout << this->token_history[i] << " ";
    // }
    // std::cout << std::endl;
    // // print tokens to insert
    // header_print("DEBUG", "Tokens to insert: ");
    // for (size_t i = 0; i < tokens.size(); i++) {
    //     std::cout << tokens[i] <<  " ";
    // }
    // std::cout << std::endl;

    // prefix check for tokens and token history to see if we can skip some tokens
    const size_t idx = this->token_history.size();
    size_t skip_count = 0;
    for (size_t i = 0; i < idx; i++) {
        if (tokens[i] == this->token_history[i]) {
            skip_count++;
        }
        else {
            break;
        }
    }
    if (skip_count != idx) {
        clear_context();
        skip_count = 0;
    }
    tokens.erase(tokens.begin(), tokens.begin() + skip_count);


    if (this->total_tokens + tokens.size() >= this->MAX_L){
        header_print("WARNING", "Max length reached, stopping prefilling...");
        return false;
    }
    for (int token : tokens){
        this->token_history.push_back(token);
    }
    buffer<bf16> y;

    auto prefill_start_time = this->profiler_list[PREFILL_TIME].start();

    y = _chunked_insert(meta_info, tokens, is_cancelled, payload, first_len_run);

    auto prefill_end_time = this->profiler_list[PREFILL_TIME].stop(tokens.size());
    meta_info.prefill_duration = (uint64_t)time_utils::duration_ns(prefill_start_time, prefill_end_time).first;
    meta_info.prompt_tokens = tokens.size();

    if (meta_info.stop_reason == CANCEL_DETECTED) {
        return false;
    }

    this->total_tokens += tokens.size();
    if (this->total_tokens >= this->MAX_L){
        header_print("WARNING", "Max length reached, stopping prefilling...");
    }
    this->profiler_list[SAMPLING_TIME].start();
    this->last_token = this->sampler->sample(y);
    this->profiler_list[SAMPLING_TIME].stop(1);
    return true;
}

buffer<bf16> AutoModel::_chunked_insert(chat_meta_info_t& meta_info, std::vector<int>& tokens, std::function<bool()> is_cancelled, void* payload, int first_len_run) {
    int max_prefill_len = meta_info.max_prefill_len;
    // make max_prefill_len a 2^n
    max_prefill_len = 1 << static_cast<int>(std::ceil(std::log2(max_prefill_len)));
    buffer<bf16> y;
    if (max_prefill_len < 512) {
        y = this->lm_engine->prefill(tokens, payload);
    }
    else{
        if (first_len_run > 0) {
            int new_max_len = 1 << static_cast<int>(std::ceil(std::log2(first_len_run)));
            if (new_max_len > max_prefill_len) {
                max_prefill_len = new_max_len;
            }
        }
        int chunks = (tokens.size() + max_prefill_len - 1) / max_prefill_len;
        for (int i = 0; i < chunks; i++) {
            if (is_cancelled()) {
                meta_info.stop_reason = CANCEL_DETECTED;
                // reset stream content
                buffer_.clear();
                current_mode_ = StreamEventType::CONTENT;
                tool_name_.clear();
                is_in_tool_block_ = false;
                break;
            }
            int start = i * max_prefill_len;
            int end = std::min(static_cast<int>(tokens.size()), (i + 1) * max_prefill_len);
            std::vector<int> chunk_tokens(tokens.begin() + start, tokens.begin() + end);
            header_print("FLM", "Prefill chunk " + std::to_string(i+1) + "/" + std::to_string(chunks) + " with " + std::to_string(chunk_tokens.size()) + " tokens");
            buffer<bf16> chunk_y = this->lm_engine->prefill(chunk_tokens, (i == 0)? payload : nullptr);
            if (i == chunks - 1) {
                y = chunk_y;
            }
        }
    }
    return y;
}

std::string AutoModel::_shared_generate(chat_meta_info_t& meta_info, int length_limit, std::ostream& os, std::function<bool()> is_cancelled) {
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
    this->token_history.push_back(this->last_token);
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
    return result;
}

StreamResult AutoModel::_shared_think_tool_calling_pasrsed(const std::string content) {
    const std::string MARKER_THINK_START = "<think>";
    const std::string MARKER_THINK_END = "</think>";
    const std::string MARKER_TOOL_START = "<tool_call>";
    const std::string MARKER_TOOL_END = "</tool_call>";

    StreamResult result;
    buffer_ += content;

    while (true) {
        // check tool calling
        if (!is_in_tool_block_) {
            size_t tool_start_pos = buffer_.find(MARKER_TOOL_START);
            if (tool_start_pos != std::string::npos) {
                // find the <tool_call> and output the content before it
                if (tool_start_pos > 0) {
                    result.content = buffer_.substr(0, tool_start_pos);
                    result.type = current_mode_;
                    buffer_ = buffer_.substr(tool_start_pos);
                    return result;
                }

                // go to tool calling
                is_in_tool_block_ = true;
                tool_name_.clear();
                buffer_ = buffer_.substr(MARKER_TOOL_START.length());
                result.type = StreamEventType::WAITING;
                return result;
            }
        }

        // tool calling process
        if (is_in_tool_block_) {
            size_t tool_end_pos = buffer_.find(MARKER_TOOL_END);
            if (tool_end_pos != std::string::npos) {
                tool_name_ += buffer_.substr(0, tool_end_pos);
                buffer_ = buffer_.substr(tool_end_pos + MARKER_TOOL_END.length());
                is_in_tool_block_ = false;

                try {
                    auto j = nlohmann::json::parse(tool_name_);

                    result.type = StreamEventType::TOOL_DONE;
                    result.tool_id = "generate_id()";

                    if (j.contains("name")) {
                        result.tool_name = j["name"].get<std::string>();
                    }

                    if (j.contains("arguments")) {
                        if (j["arguments"].is_string()) {
                            result.tool_args_str = j["arguments"].get<std::string>();
                        }
                        else {
                            result.tool_args_str = j["arguments"].dump();
                        }
                    }

                    return result;
                }
                catch (...) {
                    result.type = StreamEventType::CONTENT;
                    result.content = "[Error parsing tool call]";
                    return result;
                }
            }
            else {
                tool_name_ += buffer_;
                buffer_.clear();
                result.type = StreamEventType::WAITING;
                return result;
            }
        }

        // normal and reasoning
        if (current_mode_ == StreamEventType::CONTENT) {
            size_t think_start_pos = buffer_.find(MARKER_THINK_START);

            if (think_start_pos != std::string::npos) {
                if (think_start_pos > 0) {
                    result.content = buffer_.substr(0, think_start_pos);
                    result.type = StreamEventType::CONTENT;
                    buffer_ = buffer_.substr(think_start_pos);
                    return result;
                }
                buffer_ = buffer_.substr(MARKER_THINK_START.length());
                current_mode_ = StreamEventType::REASONING;
                continue;
            }
        }
        else if (current_mode_ == StreamEventType::REASONING) {
            size_t think_end_pos = buffer_.find(MARKER_THINK_END);

            if (think_end_pos != std::string::npos) {
                if (think_end_pos > 0) {
                    result.content = buffer_.substr(0, think_end_pos);
                    result.type = StreamEventType::REASONING;
                    buffer_ = buffer_.substr(think_end_pos);
                    return result;
                }
                buffer_ = buffer_.substr(MARKER_THINK_END.length());
                current_mode_ = StreamEventType::CONTENT;
                continue;
            }
        }

        if (!buffer_.empty()) {
            result.content = buffer_;
            result.type = current_mode_;
            buffer_.clear();
            return result;
        }

        break;
    }

    result.type = current_mode_;
    return result;
}

/// \brief Clear the context
/// \note The function will clear the context
/// \note The function will reset the total tokens
/// \note The function will reset the last token
/// \note The function will clear the context
/// \note The function will reset the total tokens
void AutoModel::clear_context() {
    this->total_tokens = 0;
    this->last_token = -1;
    this->token_history.clear();
    this->checkpoint_his.clear();
    this->lm_engine->clear_context();
    this->total_tokens = 0;
    this->sampler->reset_penalties();
    for (size_t i = 0; i < PROFILER_TYPE_NUM; i++) {
        this->profiler_list[i].reset();
    }
    this->last_prefill_time = { 0, "us" };
}


/// \brief Get the current context length
/// \note The function will get the current context length
/// \note The function will return the current context length
int AutoModel::get_current_context_length() {
    return this->total_tokens;
}

/// \brief Set the sampler
/// \param sampler_config the sampler config
/// \note The function will set the sampler
/// \note The function will reset the sampler
/// \note The function will set the sampler config
void AutoModel::set_sampler(sampler_config& sampler_config) {
    if (this->sampler != nullptr) {
        this->sampler.reset();
    }
    this->sampler = std::make_unique<Sampler>(this->lm_config->vocab_size, sampler_config);
}

/// \brief Set the max length
/// \param MAX_L the max length
/// \note The function will set the max length
/// \note The function will update the max length
void AutoModel::set_max_length(unsigned int MAX_L) {
    this->MAX_L = std::max(MAX_L, this->MAX_L);
    if (this->lm_engine != nullptr) {
        this->lm_engine->update_max_length(MAX_L);
    }
}

/// \brief Show the model info
/// \note The function will show the model info
/// \note The function will return the model info
std::string AutoModel::show_model_info() {
    try {
        std::string ss = this->lm_config->_str();
        return ss;
    }
    catch (const std::exception& e) {
        return "Error showing model info: " + std::string(e.what());
    }
}

/// \brief Show the profile
/// \note The function will show the profile
/// \note The function will return the profile
std::string AutoModel::show_profile() {
    std::stringstream ss;
    int total_tokens = this->lm_engine->get_current_context_length();
    time_utils::time_with_unit time = this->profiler_list[TOTAL_TIME].get_total_time();
    ss << "  Statistics:" << std::endl;
    ss << "    Total tokens:        " << this->get_current_context_length() << " (" << total_tokens << ")" << std::endl;
    ss << "    Total time:          " << time.first << " " << time.second << std::endl;
    time = this->profiler_list[DECODING_TIME].get_total_time();
    ss << "    Decoding time:       " << time.first << " " << time.second << std::endl;
    time = this->profiler_list[PREFILL_TIME].get_total_time();
    ss << "    Prefill time:        " << time.first << " " << time.second << std::endl;
    // time = this->profiler_list[SAMPLING_TIME].get_total_time();
    // ss << "    Sampling time:       " << time.first << " " << time.second << std::endl;
    // time = this->profiler_list[TKOEN_ENCODE_TIME].get_total_time();
    // ss << "    Token encoding time: " << time.first << " " << time.second << std::endl;
    // time = this->profiler_list[TKOEN_DECODE_TIME].get_total_time();
    // ss << "    Token decoding time: " << time.first << " " << time.second << std::endl;
    ss << "    Average decoding speed:       " << this->profiler_list[DECODING_TIME].get_average_speed() << " tokens/s" << std::endl;
    ss << "    Average prefill  speed:       " << this->profiler_list[PREFILL_TIME].get_average_speed() << " tokens/s" << std::endl;
    // ss << "    Average sampling speed:       " << this->profiler_list[SAMPLING_TIME].get_average_speed() << " tokens/s" << std::endl;
    // ss << "    Average token encoding speed: " << this->profiler_list[TKOEN_ENCODE_TIME].get_average_speed() << " tokens/s" << std::endl;
    // ss << "    Average token decoding speed: " << this->profiler_list[TKOEN_DECODE_TIME].get_average_speed() << " tokens/s" << std::endl;
    // ss << "    Average overall speed:        " << this->profiler_list[TOTAL_TIME].get_average_speed() << " tokens/s" << std::endl;

    return ss.str();
}

/// \brief Get the history
/// \note The function will get the history
/// \note The function will return the history
std::pair<std::string, std::vector<int>> AutoModel::get_history() {
    std::vector<int> history = this->token_history;
    std::string all_context = this->tokenizer->decode(history);
    return std::make_pair(all_context, history);
}

/// \brief Verbose
/// \note The function will verbose
/// \note The function will print the verbose
void AutoModel::verbose() {
    std::cout << std::endl;
    int total_tokens = this->get_current_context_length();
    float prefill_speed = this->profiler_list[PREFILL_TIME].get_average_speed();
    float decoding_speed = this->profiler_list[DECODING_TIME].get_average_speed();
    time_utils::time_with_unit ttft_time = this->profiler_list[TTFT_TIME].get_total_time();
    float context_percentage = (float)total_tokens / (float)this->MAX_L * 100;
    std::cout << "Verbose: " << std::endl;
    std::cout << "  Total tokens:        " << total_tokens << " (" << std::fixed << std::setprecision(2) << context_percentage << "%)" << std::endl;
    std::cout << "  TTFT:                " << ttft_time.first << " " << ttft_time.second << std::endl;
    std::cout << "  Prefill speed:       " << std::fixed << std::setprecision(2) << prefill_speed << " tokens/s" << std::endl;
    std::cout << "  Decoding speed:      " << std::fixed << std::setprecision(2) << decoding_speed << " tokens/s" << std::endl << std::endl;
}

/// \brief Set the top-k
/// \param topk the top-k
/// \note The function will set the top-k
/// \note The function will check if the top-k is valid
void AutoModel::set_topk(int topk) {
    if (topk < 1) {
        header_print("WARNING", "Top-k must be greater than 0");
        return;
    }
    if (topk > this->lm_config->vocab_size) {
        header_print("WARNING", "Top-k is greater than vocab size, set to vocab size: " << this->lm_config->vocab_size);
        topk = this->lm_config->vocab_size;
    }

    this->sampler->top_k = topk;
}

/// \brief Set the top-p
/// \param topp the top-p
/// \note The function will set the top-p
/// \note The function will check if the top-p is valid
void AutoModel::set_topp(float topp) {
    if (topp < 0.0f || topp > 1.0f) {
        header_print("WARNING", "Top-p must be between 0.0 and 1.0");
        return;
    }
    this->sampler->top_p = topp;
}

/// \brief Set the min-p
/// \param topp the min-p
/// \note The function will set the min-p
/// \note The function will check if the min-p is valid
void AutoModel::set_minp(float minp) {
    if (minp < 0.0f || minp > 1.0f) {
        header_print("WARNING", "Min-p must be between 0.0 and 1.0");
        return;
    }
    this->sampler->min_p = minp;
}

/// \brief Set the temperature
/// \param temperature the temperature
/// \note The function will set the temperature
/// \note The function will check if the temperature is valid
void AutoModel::set_temperature(float temperature) {
    if (temperature < 0.0f) {
        header_print("WARNING", "Temperature must be greater than 0.0");
        return;
    }
    this->sampler->temperature = temperature;
}

/// \brief Set the presence penalty
/// \param presence_penalty the presence penalty
/// \note The function will set the presence penalty
/// \note The function will check if the presence penalty is valid
void AutoModel::set_presence_penalty(float presence_penalty) {
    if (presence_penalty < 0.0f) {
        header_print("WARNING", "Presence penalty must be greater than 0.0");
        return;
    }
    this->sampler->pre_penalty = presence_penalty;
}

/// \brief Set the repetition penalty
/// \param repetition_penalty the repetition penalty
/// \note The function will set the repetition penalty
/// \note The function will check if the repetition penalty is valid
void AutoModel::set_repetition_penalty(float repetition_penalty) {
    if (repetition_penalty < 0.0f) {
        header_print("WARNING", "Repetition penalty must be greater than 0.0");
        return;
    }
    if (repetition_penalty < 1.0f) {
        header_print("WARNING", "If Repetition Penalty < 1.0, it will reward previous generated tokens, which may cause infinite loop!");
    }
    this->sampler->rep_penalty = repetition_penalty;
}

/// \brief Set the frequency penalty
/// \param frequency_
void AutoModel::set_frequency_penalty(float frequency_penalty) {
    if (frequency_penalty < 0.0f) {
        header_print("WARNING", "Frequency penalty must be greater than 0.0");
        return;
    }
    this->sampler->freq_penalty = frequency_penalty;
}

/// \brief Set the frequency penalty window
/// \param frequency_penalty_window the frequency penalty window
/// \note The function will set the frequency penalty window
/// \note The function will check if the frequency penalty window is valid
void AutoModel::set_frequency_penalty_window(int frequency_penalty_window) {
    this->sampler->freq_penalty_window = frequency_penalty_window;
}

/// \brief Set the penalty window
/// \param penalty_window the frequency penalty window
/// \note The function will set the penalty window
/// \note The function will check if the penalty window is valid
void AutoModel::set_penalty_window(int penalty_window) {
    this->sampler->repeat_last_n = penalty_window;
}

/// \brief Start the ttft timer
/// \note The function will start the ttft timer
/// \note The function will reset the ttft timer
void AutoModel::start_ttft_timer() {
    this->profiler_list[TTFT_TIME].reset();
    this->profiler_list[TTFT_TIME].start();
}

/// \brief Stop the ttft timer
/// \note The function will stop the ttft timer
/// \note The function will check if the ttft timer is valid
void AutoModel::stop_ttft_timer() {
    this->profiler_list[TTFT_TIME].stop(1);
}

/// \brief Reset the total timer
/// \note The function will reset the total timer
/// \note The function will check if the total timer is valid
void AutoModel::reset_total_timer() {
    this->profiler_list[TOTAL_TIME].reset();
}

/// \brief Start the total timer
/// \note The function will start the total timer
/// \note The function will check if the total timer is valid
void AutoModel::start_total_timer() {
    this->profiler_list[TOTAL_TIME].start();
}

/// \brief Stop the total timer
/// \note The function will stop the total timer
/// \note The function will check if the total timer is valid
void AutoModel::stop_total_timer() {
    this->profiler_list[TOTAL_TIME].stop(this->total_tokens, true);
}

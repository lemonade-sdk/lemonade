/// \file whisper_npu.hpp
/// \brief whisper_npu class
/// \author FastFlowLM Team
/// \date 2025-10-17
/// \version 0.9.24
/// \note This is a source file for the modeling_whisper class
#include "whisper/modeling_whisper.hpp"


Whisper::Whisper(xrt::device* npu_device_inst){
    this->device = npu_device_inst;

    time_stamp = 0;
    audio_buffer.clear();
    fft_400 = std::make_unique<FFT400>();
    mel_feature = buffer<bf16>(128 * 3000);
    this->profiler_list.resize(PROFILER_TYPE_NUM);
    for (size_t i = 0; i < PROFILER_TYPE_NUM; i++) {
        this->profiler_list[i] = profiler();
        this->profiler_list[i].reset();
    }
    this->last_prefill_time = { 0, "us" };
}

void Whisper::load_model(std::string model_path, nlohmann::ordered_json model_info, bool enable_preemption) {
    header_print("FLM", "Loading model: " << model_path);
    this->npu = std::make_unique<npu_xclbin_manager>(npu_device::device_npu2, this->device, enable_preemption);
    this->enable_preemption = enable_preemption;
    this->model_path = model_path;
    this->enable_preemption = enable_preemption;

    this->lm_config = std::make_unique<Whisper_Config>();
    this->lm_config->from_pretrained(this->model_path);

    this->whisper_engine = std::make_unique<whisper_npu>(*this->lm_config, this->npu.get(), 448);
    std::unique_ptr<Q4NX> q4nx = std::make_unique<Q4NX>(this->model_path);

    this->whisper_engine->load_weights(*q4nx);

    // //free the q4nx
    q4nx.reset();
    this->whisper_engine->clear_context();
    this->setup_tokenizer(model_path);

    this->sampler.reset();

    sampler_config s_config;
    s_config.top_k = 1;
    s_config.top_p = 0.95;
    s_config.min_p = 0.1;
    s_config.temperature = 0.4;

    this->sampler = std::make_unique<Sampler>(this->lm_config->vocab_size, s_config);
}

void Whisper::setup_tokenizer(std::string model_path) {
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

    this->tokenizer = std::make_unique<Tokenizer>(this->model_path);
    this->_build_time_map();
}

bool Whisper::load_audio(std::string& audio_path) {
    this->audio_buffer = this->_load_audio(audio_path);
    header_print("info", "Length of audio: " + std::to_string(_S2T_(this->audio_buffer.size())) + " seconds");
    return true;
}

bool Whisper::load_audio(std::vector<uint8_t>& audio_data) {
    this->audio_buffer = this->_load_audio(audio_data);
    header_print("info", "Length of audio: " + std::to_string(_S2T_(this->audio_buffer.size())) + " seconds");
    return true;
}

std::pair<std::string, std::string> Whisper::generate(whisper_task_type_t task, bool enable_time_stamp, bool return_time_stamp, std::ostream& os) {
    int length = this->audio_buffer.size();
    int current_idx = 0;
    int overlapping_samples = 5 * FS; //
    int l_this_round = std::min(WINDOW_SAMPLES, length);
    int last_time_stamp = this->token_time_map_offset;
    int last_idx;
    bool last_chunk = false;
    std::string result;
    std::string language_detected;
    if ((!enable_time_stamp) && (return_time_stamp)){
        header_print("Error", "Return_time_stamp is true but timestamp is not enabled!");
        return std::make_pair("", "");
    }
    while (current_idx < length){
        bool allow_force_time_stamp = true;
        // std::cout << "Chunk " << _S2T_(current_idx) << "s to " << _S2T_(current_idx + l_this_round) << "s" << std::endl;
        float time_offset = _S2T_(current_idx);

        if (l_this_round == 0){
            break;
        }

        std::vector<float> audio_chunk(l_this_round);

        audio_chunk.insert(audio_chunk.begin(), this->audio_buffer.data() + current_idx, this->audio_buffer.data() + current_idx + l_this_round);

        _preprocess_audio(mel_feature, audio_chunk);

        // run whisper encoder
        this->whisper_engine->encode_audio(mel_feature); // encoded and pass kv-cache to decoder

        // decoder loop
        this->whisper_engine->clear_context();
        this->sampler->reset_penalties();

        last_idx = start_of_transcript; // the first token is fixed
        buffer<bf16> logits = this->whisper_engine->decode_audio(last_idx);
        last_idx = this->_sample_in_language(logits);

        // std::cout << "Language detected: " << this->tokenizer->run_time_decoder(last_idx) << "(" << langmap::to_language_name(this->tokenizer->run_time_decoder(last_idx)) << ")" << std::endl;
        language_detected = this->tokenizer->run_time_decoder(last_idx);

        if (task == e_translate) {
            //header_print("info", "translate is not supported! Do transcribe instead!");
            //task = e_transcribe;
            //buffer<bf16> logits = this->whisper_engine->decode_audio(50259); // en
            //logits = this->whisper_engine->decode_audio(translate_token);
            //last_idx = this->_sample_in_time_stamp(logits);
        }
        else if (task == e_transcribe) {
            buffer<bf16> logits = this->whisper_engine->decode_audio(transcribe_token);
            last_idx = this->_sample_in_time_stamp(logits);
        }
        else {
            header_print("Error", "Non-recongnized task!");
        }

        if (enable_time_stamp){
            if (return_time_stamp){
                std::string time_stamp = this->tokenizer->run_time_decoder(last_idx);
                std::string offset_time_stamp = this->_offset_time_stamp(time_stamp, time_offset);
                result += offset_time_stamp;
                os << offset_time_stamp << std::flush;
            }
        }
        else{
            buffer<bf16> logits = this->whisper_engine->decode_audio(no_time_stamp_token);
            last_idx = this->sampler->sample(logits);
            std::string token_str = this->tokenizer->run_time_decoder(last_idx);
            result += token_str;
            os << token_str << std::flush;
        }

        int watching_dog = 16;
        for (int i = 0; i < 448 - 3; i++){
            buffer<bf16> logits = this->whisper_engine->decode_audio(last_idx);
            if (watching_dog == 0 && allow_force_time_stamp){
                last_idx = this->_sample_in_time_stamp(logits);
                watching_dog = 16;
            }
            else{
                last_idx = this->sampler->sample(logits);
                if (watching_dog > 0){
                    watching_dog--;
                }
            }
            std::string token_str = this->tokenizer->run_time_decoder(last_idx);

            if (_is_normal_token(last_idx)){
                os << token_str << std::flush;
                result += token_str;
            }
            else if (return_time_stamp && _is_time_stemp(last_idx)){
                std::string offset_time_stamp = this->_offset_time_stamp(token_str, time_offset);
                os << offset_time_stamp << std::flush;
                result += offset_time_stamp;
            }

            if (enable_time_stamp && _is_time_stemp(last_idx)){
                last_time_stamp = last_idx;
                watching_dog = 16;
            }

            if (last_idx == 50257){
                break;
            }

            if (!_is_valid_utf8(token_str)){
                allow_force_time_stamp = false;
            }
            else {
                allow_force_time_stamp = true;
            }
        }


        if (l_this_round < WINDOW_SAMPLES){
            break;
        }

        if (enable_time_stamp){
            float end_time = _get_time(last_time_stamp);
            if (end_time == 0){
                end_time = 30;
            }
            l_this_round = end_time * FS;
        }


        current_idx += l_this_round;

        l_this_round = std::min(WINDOW_SAMPLES, length - current_idx);
        l_this_round = std::max(l_this_round, 0);

    }
    return std::make_pair(result, langmap::to_language_name(language_detected));
}


void Whisper::_build_time_map(){
    this->token_time_map.clear();
    auto ids = this->tokenizer->encode("<|0.00|>");
    this->token_time_map_offset = ids[0];
    for (float time = 0; time <= 30.00; time += 0.02){
        this->token_time_map.push_back(time);
    }

    this->total_time_stamps = this->token_time_map.size();
}

int Whisper::_sample_in_language(buffer<bf16>& logits){

    for (int i = 0; i < 50259; i++){
        logits[i] = -0x1.FEp127f;
    }
    for (int i = 50359; i < logits.size(); i++){
        logits[i] = -0x1.FEp127f;
    }
    return this->sampler->sample(logits);

}

int Whisper::_sample_in_time_stamp(buffer<bf16>& logits){
    for (int i = 0; i < this->token_time_map_offset; i++){
        logits[i] = -0x1.FEp127f;
    }
    for (int i = this->total_time_stamps + this->token_time_map_offset; i < logits.size(); i++){
        logits[i] = -0x1.FEp127f;
    }
    return this->sampler->sample(logits);
}

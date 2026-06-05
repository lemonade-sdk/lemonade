/*
*  Copyright (c) 2025 by Contributors
*  \file runner.cpp
*  \brief Runner implementation for interactive model execution
*  \author FastFlowLM Team
*  \date 2025-08-05
*  \version 0.9.21
*/
#include "runner.hpp"
#include "harmony_filter.hpp"
#ifndef FASTFLOWLM_LINUX_LIMITED_MODELS
#include "AutoEmbeddingModel/all_embedding_model.hpp"
#endif
#include <iostream>
#include <sstream>
#include <filesystem>
#include <map>
#include <iomanip>
#include <fstream>
#include <algorithm>

/// \brief Command map for command line input
std::map<std::string, runner_cmd_t> cmd_map = {
    {"/set", CMD_SET},
    {"/show", CMD_SHOW},
    {"/load", CMD_LOAD},
    {"/save", CMD_SAVE},
    {"/clear", CMD_CLEAR},
    {"/bye", CMD_BYE},
    {"/pull", CMD_PULL},
    {"/?", CMD_HELP},
    {"/? shortcuts", CMD_HELP_SHOTCUT},
};

/// \brief Constructor
/// \param supported_models - the list of supported models
/// \param downloader - the downloader for the models
/// \param tag - the tag of the model to load
Runner::Runner(model_list& supported_models, ModelDownloader& downloader, program_args_t& args)
    : supported_models(supported_models), downloader(downloader), tag(args.model_tag), asr(args.asr), embed(args.embed), img_pre_resize(args.img_pre_resize), preemption(args.preemption) {

    this->npu_device_inst = xrt::device(0);

    if (args.ctx_length != -1) {
        this->ctx_length = args.ctx_length >= 512 ? args.ctx_length : 512;
    }
    else {
        this->ctx_length = -1;
    }

    if (this->auto_chat_engine != nullptr) {
        this->auto_chat_engine.reset();
    }
    std::pair<std::string, std::unique_ptr<AutoModel>> auto_model = get_auto_model(this->tag, this->supported_models, &this->npu_device_inst);
    this->auto_chat_engine = std::move(auto_model.second);

    this->tag = auto_model.first;

    if (!this->downloader.is_model_downloaded(this->tag)) {
        this->downloader.pull_model(this->tag);
    }
    auto [new_tag, model_info] = this->supported_models.get_model_info(this->tag);
    this->asr_supported = model_info.contains("asr") && model_info["asr"];
    // header_print("ASR", asr_supported);
    this->auto_chat_engine->configure_parameter("img_pre_resize", this->img_pre_resize);
    try {
        this->auto_chat_engine->load_model(this->supported_models.get_model_path(new_tag), model_info, this->ctx_length, this->preemption);
    }
    catch (const std::exception& e) {
        header_print("ERROR", "Failed to load model: " + std::string(e.what()));
        exit(EXIT_FAILURE);
    }

    try {
        if (args.prefill_chunk_len != -1) {
            this->prefill_chunk_len = args.prefill_chunk_len >= 512 ? args.prefill_chunk_len : 512;
        }
        else {
            this->prefill_chunk_len = model_info["max_prefill_len"].get<int>();;
        }
    }
    catch (const std::exception& e) {
        this->prefill_chunk_len = 4096; // default value
    }

    this->generate_limit = -1;


    if (this->embed) {
        header_print("Warning", "Embed model not supported in CLI; Use 'flm serve -e 1'");
    }

    header_print("FLM", "Loading model: " << this->tag);

#ifndef FASTFLOWLM_LINUX_LIMITED_MODELS
    if (this->asr) {
        // check if multi-modal model with ASR support is loaded, if not, load the default whisper model for ASR
        if (!asr_supported)
        {
            header_print("FLM", "The loaded model does not support ASR. Loading default Whisper model for ASR...");
            std::string whisper_tag = "whisper-v3:turbo";
            if (!this->downloader.is_model_downloaded(whisper_tag)) {
                this->downloader.pull_model(whisper_tag);
            }
            this->whisper_engine = std::make_unique<Whisper>(&this->npu_device_inst);
            auto [new_whisper_tag, whisper_model_info] = this->supported_models.get_model_info(whisper_tag);
            std::string whisper_model_path = this->supported_models.get_model_path(new_whisper_tag);
            try {
                this->whisper_engine->load_model(whisper_model_path, whisper_model_info, this->preemption);
            }
            catch (const std::exception& e) {
                header_print("ERROR", "Failed to load ASR model: " + std::string(e.what()));
                exit(EXIT_FAILURE);
            }
        }
        else {
            header_print("FLM", "The loaded model (" + this->tag + ") already supports ASR. No additional ASR model is needed.");
        }
    }
#else
    if (this->asr) {
        header_print("Error", "ASR models are not supported in this build");
    }
#endif
}





/// \brief Run the runner
void Runner::run() {
    bool verbose = false;
    this->system_prompt = "";
    this->auto_chat_engine->configure_parameter("system_prompt", this->system_prompt);
    std::wstring_convert<std::codecvt_utf8<wchar_t>> utf8conv;
    wstream_buf obuf(std::cout);
    std::ostream base_ostream(&obuf);
    header_print("FLM", "Type /? for help");
    int empty_line_count = 0;
    bool is_image = false;
    bytes image;
    while (true) {
        std::string input = cli.get_interactive_input();
        is_image = false;
        if (input.empty()) {
            empty_line_count++;
            if (empty_line_count > 2) {
                header_print("FLM", "Type /? for help");
                empty_line_count = 0;
            }
            continue;
        }

        // Convert line (UTF-8 bytes) → wide string (Unicode codepoints)
        std::wstring winput = utf8conv.from_bytes(input);

        // Split on *any* Unicode whitespace
        std::wistringstream wiss(winput);
        std::wstring wtoken;
        std::vector<std::string> input_list;
        std::cout << std::endl;

        while (wiss >> wtoken) {
            // Convert each token back → UTF-8 bytes
            input_list.push_back(utf8conv.to_bytes(wtoken));
        }

        // now input_list contains your UTF-8 tokens
        if (input_list.empty()) {
            continue;
        }

        // For commands, we only need to check the first token
        std::string first_token = input_list[0];


        // Check if this is a command (starts with /)
        bool is_command = (first_token[0] == '/');

        if (is_command && first_token != "/input") {
            if (first_token == "/bye") {
                break;
            }
            else if (first_token == "/clear") {
                this->cmd_clear(input_list);
            }
            else if (first_token == "/status") {
                this->cmd_status(input_list);
            }
            else if (first_token == "/load") {
                this->cmd_load(input_list);
            }
            else if (first_token == "/save") {
                this->cmd_save(input_list);
            }
            else if (first_token == "/show") {
                this->cmd_show(input_list);
            }
            else if (first_token == "/set") {
                this->cmd_set(input_list);
            }
            else if (first_token == "/list") {
                std::cout << "Models:" << std::endl;
                nlohmann::json models = supported_models.get_all_models();
                for (const auto& model : models["models"]) {
                    bool is_present = downloader.is_model_downloaded(model["name"].get<std::string>());
                    std::cout << "  - " << model["name"].get<std::string>();
                    if (is_present){
                        std::cout << " ✅";
                    }
                    else{
                        std::cout << " ⏬";
                    }
                    std::cout << std::endl;
                }
            }
            else if (first_token == "/think") {
                if (!this->auto_chat_engine->configure_parameter("toggle_think", std::any{})) {
                    header_print("WARNING", "Think is not toggleable for this model!");
                }
            }
            else if (first_token == "/help") {
                this->cmd_help(input_list);
            }
            else if (first_token == "/?") {
                this->cmd_help(input_list);
            }
            else if (first_token == "/verbose") {
                verbose = !verbose;
            }
            else if (first_token == "/history") {
                std::pair<std::string, std::vector<int>> history = this->auto_chat_engine->get_history();
                std::cout << "History: " << std::endl;
                std::cout << history.first << std::endl;
                std::cout << "Tokens: " << history.second.size() << std::endl;
                for (int i = 0; i < history.second.size(); i++) {
                    std::cout << std::dec << history.second[i] << " ";
                }
                std::cout << std::endl;
            }
            else if (first_token == "/pull") {
                std::string model_name = input_list[1];
                this->downloader.pull_model(model_name);
            }
        } else {
            // This is a regular message, not a command
            // std::cout << std::endl;  // Add newline before AI response
            lm_uniform_input_t uniformed_input;
            int last_file_name_idx = 0;
            std::string audio_context = "";
            if (first_token == "/input") {
                input = "\n";
                std::string filename;
                if (input_list[1][0] == '\"'){
                    for (int i = 1; i < input_list.size(); i++) {
                        filename += input_list[i];
                        if (input_list[i][input_list[i].size() - 1] == '\"') {
                            last_file_name_idx = i;
                            break;
                        }
                        filename += " ";
                    }
                    filename = filename.substr(1, filename.size() - 2);
                }
                else{
                    filename = input_list[1];
                    last_file_name_idx = 1;
                }

                header_print("FLM", "Loading file: " << filename);
                std::cout << std::endl;
                if (filename.find(".jpg") != std::string::npos || filename.find(".png") != std::string::npos || filename.find(".jpeg") != std::string::npos) {
                    uniformed_input.images.push_back(filename);
                    uniformed_input.image_payload_types.push_back(FILE_NAME);
                }
                else if (filename.find(".wav") != std::string::npos || filename.find(".mp3") != std::string::npos || filename.find(".ogg") != std::string::npos || filename.find(".m4a") != std::string::npos) {
#ifndef FASTFLOWLM_LINUX_LIMITED_MODELS
                    if (this->asr_supported) {
                        // gemma4 process
                        uniformed_input.audios.push_back(filename);
                        uniformed_input.audio_payload_types.push_back(FILE_NAME);
                    }
                    else if (this->asr) {
                        // check if the file exists
                        if (!std::filesystem::exists(filename)) {
                            header_print("FLM", "Error: Could not open file: " << filename);
                            header_print("FLM", "Please check if the file exists and is readable.");
                            continue;
                        }
                        this->whisper_engine->load_audio(filename);
                        std::cout << "Audio content: " << std::flush;
                        std::pair<std::string, std::string> audio_result = this->whisper_engine->generate(Whisper::whisper_task_type_t::e_transcribe, true, false, std::cout);
                        audio_context = audio_result.first;
                        std::cout << std::endl;
                        input = "Audio content (" + audio_result.second + "): " + audio_context + "\n";
                    }
                    else {
                        header_print("Warning", "No asr model loaded, cannot load audio file");
                        continue;
                    }
#else
                    header_print("Warning", "ASR models are not supported in this build");
                    continue;
#endif

                }
                else{
#ifdef _WIN32
                    std::wifstream file(utf8_to_wstring(filename));
                    if (!file.is_open()) {
                        header_print("FLM", "Error: Could not open file: " << filename);
                        header_print("FLM", "Please check if the file exists and is readable.");
                        continue;
                    }
                    file.imbue(std::locale(file.getloc(), new std::codecvt_utf8<wchar_t>));  // treat file content as UTF-8
                    std::wstring file_content_original((std::istreambuf_iterator<wchar_t>(file)), std::istreambuf_iterator<wchar_t>());
                    std::string file_content = utf8conv.to_bytes(file_content_original);
                    file.close();
#else
                    std::ifstream file(filename);
                    if (!file.is_open()) {
                        header_print("FLM", "Error: Could not open file: " << filename);
                        header_print("FLM", "Please check if the file exists and is readable.");
                        continue;
                    }
                    std::string file_content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                    file.close();
#endif
                    input = file_content + "\n";
                    std::cout << std::endl;
                }
                if (last_file_name_idx < input_list.size() - 1) {
                    for (int i = last_file_name_idx + 1; i < input_list.size() - 1; i++) {
                        input += input_list[i] + " ";
                    }
                    input += input_list[input_list.size() - 1];
                }
            }

            chat_meta_info_t meta_info;
            meta_info.max_prefill_len = this->prefill_chunk_len;
            uniformed_input.prompt = input;

            this->auto_chat_engine->start_total_timer();

            this->auto_chat_engine->start_ttft_timer();
            try {
                bool success = this->auto_chat_engine->insert(meta_info, uniformed_input);
                if (!success){
                    header_print("WARNING", "Max length reached, stopping generation...");
                    break;
                }
            }
            catch (const std::exception& e) {
                header_print("ERROR", "Insertion error: " + std::string(e.what()));
                this->auto_chat_engine->clear_context();
                continue;
            }

            this->auto_chat_engine->stop_ttft_timer();

            // Use harmony filter for gpt-oss models
            if (this->auto_chat_engine->get_current_model().find("gpt-oss") != std::string::npos) { // contains gpt-oss:20b and gpt-oss
                cli_harmony_filter harmony_filter_ostream(std::cout);
                try {
                    this->auto_chat_engine->generate(meta_info, this->generate_limit, harmony_filter_ostream);
                }
                catch (const std::exception& e) {
                    header_print("ERROR", "Generation error: " + std::string(e.what()));
                    this->auto_chat_engine->clear_context();
                    continue;
                }
            } else {
                try {
                    this->auto_chat_engine->generate(meta_info, this->generate_limit, base_ostream);
                }
                catch (const std::exception& e) {
                    header_print("ERROR", "Generation error: " + std::string(e.what()));
                    this->auto_chat_engine->clear_context();
                    continue;
                }
            }

            this->auto_chat_engine->stop_total_timer();
            std::cout << std::endl;
            if (verbose) {
                this->auto_chat_engine->verbose();
            }
        }
    }
}

/// \brief Clear the context
/// \param input_list, std::vector<std::string>
void Runner::cmd_clear(std::vector<std::string>& input_list) {
    this->auto_chat_engine->clear_context();
}

/// \brief Show the status
/// \param input_list, std::vector<std::string>
void Runner::cmd_status(std::vector<std::string>& input_list) {
    std::cout << this->auto_chat_engine->show_profile() << std::endl;
}

/// \brief Load a model
/// \param input_list, std::vector<std::string>
void Runner::cmd_load(std::vector<std::string>& input_list) {
    std::string model_name = input_list[1];

    std::pair<std::string, std::unique_ptr<AutoModel>> auto_model = get_auto_model(model_name, this->supported_models, &this->npu_device_inst);
    model_name = auto_model.first;

    if (model_name != this->tag) {
        this->tag = model_name;

        if (!this->downloader.is_model_downloaded(this->tag)) {
            this->downloader.pull_model(this->tag);
        }
        auto_chat_engine.reset();
        if(model_name=="gpt-oss:20b")
            std::this_thread::sleep_for(std::chrono::milliseconds(2800));
        this->auto_chat_engine = std::move(auto_model.second);

        auto [new_tag, model_info] = this->supported_models.get_model_info(this->tag);
        this->auto_chat_engine->configure_parameter("img_pre_resize", this->img_pre_resize);
        try {
            this->auto_chat_engine->load_model(this->supported_models.get_model_path(new_tag), model_info, this->ctx_length, this->preemption);
        }
        catch (const std::exception& e) {
            header_print("ERROR", "Failed to load model: " + std::string(e.what()));
            exit(EXIT_FAILURE);
        }

        this->auto_chat_engine->configure_parameter("system_prompt", this->system_prompt);

    }
    else
        header_print("FLM", "Model already loaded: " << model_name);

}

/// \brief Save the history
/// \param input_list, std::vector<std::string>
void Runner::cmd_save(std::vector<std::string>& input_list) {
    std::pair<std::string, std::vector<int>> history = this->auto_chat_engine->get_history();
    // Get the FLM_MODEL_PATH environment variable for the history directory
    std::string history_dir;
    const char* path_sep = "/";
#ifdef _WIN32
    path_sep = "\\";
    char* model_path_env = nullptr;
    size_t len = 0;

    history_dir = utils::get_models_directory() + path_sep + "history";

#else
    const char* model_path_env = std::getenv("FLM_MODEL_PATH");
    if (model_path_env && *model_path_env) {
        history_dir = std::string(model_path_env) + path_sep + "history";
    } else {
        std::string documents_dir = utils::get_user_directory();
        history_dir = documents_dir + path_sep + "flm" + path_sep + "history";
    }
#endif

    // Create the history directory if it doesn't exist
    if (!std::filesystem::exists(history_dir)) {
        std::filesystem::create_directories(history_dir);
    }

    // save file to history_hh_mm_mm_dd_yyyy.txt
    // 1) get current date
    std::time_t t = std::time(nullptr);
    std::tm tm = *std::localtime(&t);

    std::ostringstream date_ss;
    date_ss << std::setw(2) << std::setfill('0') << tm.tm_hour
            << '_'
            << std::setw(2) << std::setfill('0') << tm.tm_min
            << '_'
            << std::setw(2) << std::setfill('0') << tm.tm_mon + 1
            << '_'
            << std::setw(2) << std::setfill('0') << tm.tm_mday
            << '_'
            << (tm.tm_year + 1900);
    std::string date_str = date_ss.str();

    // 2) build filename in the history directory
    std::string file_name = history_dir + path_sep + "history_" + date_str + ".txt";
    std::ofstream file(file_name);
    if (file.is_open()) {
        file << history.first << std::endl;
        file.close();
        std::cout << "History saved to " << file_name << std::endl;
    }
    else {
        std::cout << "Failed to open file: " << file_name << std::endl;
    }
}

/// \brief Show the model information
/// \param input_list, std::vector<std::string>
void Runner::cmd_show(std::vector<std::string>& input_list) {
    std::cout << this->auto_chat_engine->show_model_info() << std::endl;
    std::cout << "    max context length    : " << this->auto_chat_engine->get_max_length() << std::endl;
    std::cout << std::endl;

}

/// \brief Set the model parameters
/// \param input_list, std::vector<std::string>
void Runner::cmd_set(std::vector<std::string>& input_list) {
    // remove the /set
    if (input_list.size() < 3){
        std::cout << "Usage: /set [context] [value]" << std::endl;
        std::cout << "Available parameters: " << std::endl;
        std::cout << "  /set topk [value] - set the top-k" << std::endl;
        std::cout << "  /set topp [value] - set the top-p" << std::endl;
        std::cout << "  /set minp [value] - set the min-p" << std::endl;
        std::cout << "  /set temp [value] - set the temperature" << std::endl;
        std::cout << "  /set rep-pen [value] - set the repetition penalty" << std::endl;
        std::cout << "  /set freq-pen [value] - set the frequency penalty" << std::endl;
        std::cout << "  /set pres-pen [value] - set the presence penalty" << std::endl;
        std::cout << "  /set sys-msg [value] - set the system message" << std::endl;
        std::cout << "  /set ctx-len [value] - set the max context length" << std::endl;
        std::cout << "  /set gen-lim [value] - Limit tokens generated per round" << std::endl;
        std::cout << "  /set r-eff [low|medium|high] - set the reasoning effort level (GPT-OSS only, default = medium)" << std::endl;
        return;
    }

    std::string set_context = input_list[1];

    // Handle system_prompt specially since it can be multi-line
    if (set_context == "sys-msg") {
        // Reconstruct the original input to get the full system prompt
        std::string full_input;
        for (size_t i = 2; i < input_list.size(); i++) {
            if (i > 2) full_input += " ";
            full_input += input_list[i];
        }
        this->system_prompt = full_input;
        this->auto_chat_engine->configure_parameter("system_prompt", this->system_prompt);
        return;
    }

    // For other parameters, use the third token as the value
    if (input_list.size() < 3) {
        std::cout << "Usage: /set [context] [value]" << std::endl;
        return;
    }

    std::string set_value = input_list[2];

    if (set_context == "topk"){
        this->auto_chat_engine->set_topk(std::stoi(set_value));
    }
    else if (set_context == "topp"){
        this->auto_chat_engine->set_topp(std::stof(set_value));
    }
    else if (set_context == "minp") {
        this->auto_chat_engine->set_minp(std::stof(set_value));
    }
    else if (set_context == "temp"){
        this->auto_chat_engine->set_temperature(std::stof(set_value));
    }
    else if (set_context == "rep-pen"){
        this->auto_chat_engine->set_repetition_penalty(std::stof(set_value));
    }
    else if (set_context == "freq-pen"){
        this->auto_chat_engine->set_frequency_penalty(std::stof(set_value));
    }
    else if (set_context == "pres-pen") {
        this->auto_chat_engine->set_presence_penalty(std::stof(set_value));
    }
    else if (set_context == "ctx-len"){
        try {
            this->auto_chat_engine->set_max_length(std::stoi(set_value));
        }
        catch (const std::exception& e) {
            header_print("ERROR", "Failed to set context length: " + std::string(e.what()));
            exit(EXIT_FAILURE);
        }
    }
    else if (set_context == "gen-lim"){
        this->generate_limit = std::stoi(set_value);
    }
    else if (set_context == "prefill-chunk-len"){

    }
    else if (set_context == "r-eff"){
        if (this->auto_chat_engine->get_current_model() == "gpt-oss")
            this->auto_chat_engine->configure_parameter("reasoning_effort", set_value);
        else
            header_print("WARNING", "Reasoning effort only support for gpt-oss");
    }
    else{
        std::cout << "Invalid context: " << set_context << std::endl;
        std::cout << "Available parameters: " << std::endl;
        std::cout << "  /set ctx-len [value] - set the max context length" << std::endl;
        std::cout << "  /set topk [value] - set the top-k" << std::endl;
        std::cout << "  /set topp [value] - set the top-p" << std::endl;
        std::cout << "  /set minp [value] - set the min-p" << std::endl;
        std::cout << "  /set temp [value] - set the temperature" << std::endl;
        std::cout << "  /set rep-pen [value] - set the repetition penalty" << std::endl;
        std::cout << "  /set freq-pen [value] - set the frequency penalty" << std::endl;
        std::cout << "  /set pres-pen [value] - set the presence penalty" << std::endl;
        std::cout << "  /set sys-msg [value] - set the system message" << std::endl;
        std::cout << "  /set gen-lim [value] - Limit tokens generated per round" << std::endl;
        std::cout << "  /set r-eff [low|medium|high] - set the reasoning effort level (GPT-OSS only, default = medium)" << std::endl;
    }
}

/// \brief Show the help
/// \param input_list, std::vector<std::string>
void Runner::cmd_help(std::vector<std::string>& input_list) {
    std::cout << "Available commands:" << std::endl;
    std::cout << "  /show - show the model information" << std::endl;
    std::cout << "  /load [model_name] - load a model" << std::endl;
    std::cout << "  /input [filename] [follow_up_prompt] - load a file and follow up with a prompt" << std::endl;
    std::cout << "                                       - If space is in the filename, use quotes to wrap it" << std::endl;
    std::cout << "  /save - save the history" << std::endl;
    std::cout << "  /clear - clear the context" << std::endl;
    std::cout << "  /status - show perf. metrics" << std::endl;
    std::cout << "  /history - show the history" << std::endl;
    std::cout << "  /verbose - toggle the verbose" << std::endl;
    std::cout << "  /think - toggle the think" << std::endl;
    std::cout << "  /set [variable] [value] - set the variable" << std::endl;
    std::cout << "  /list - list all the models" << std::endl;
    std::cout << "  /bye - exit the program" << std::endl;
    std::cout << "  /? - show this help" << std::endl;
    std::cout << std::endl;
    std::cout << "Interactive input:" << std::endl;
    std::cout << "  - Press Enter to submit single-line input" << std::endl;
    std::cout << "  - Paste multi-line text and it will be detected automatically" << std::endl;
    std::cout << "  - Use 'Shift + Enter' to explicitly continue on next line" << std::endl;
    std::cout << "  - Commands (starting with /) are processed immediately" << std::endl;
}

/// \brief Show the help shotcut
/// \param input_list, std::vector<std::string>
void Runner::cmd_help_shotcut(std::vector<std::string>& input_list) {
    std::cout << "Help shotcut" << std::endl;
}

/// \file lm_config.hpp
/// \brief lm_config class
/// \author FastFlowLM Team
/// \date 2025-08-05
/// \version 0.9.10
/// \note This class is used to store the model configuration.
#pragma once

#include "typedef.hpp"
#include "utils/utils.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>

/// \brief LM_Config class
class LM_Config{
    public:
        std::string model_path;
        std::string model_name;
        std::string model_type;
        u32 head_dim;
        u32 hidden_size;
        std::string hidden_act;
        u32 intermediate_size;
        u32 num_attention_heads;
        u32 num_hidden_layers;
        u32 num_key_value_heads;
        u32 linear_conv_kernel_dim;
        u32 linear_key_head_dim;
        u32 linear_num_key_heads;
        u32 linear_num_value_heads;
        u32 linear_value_head_dim;
        u32 pretraining_tp;
        f32 rms_norm_eps;
        f32 rope_theta;
        u32 vocab_size;
        u32 sliding_window;
        u32 sliding_window_pattern;
        u32 addr_qk;
        u32 addr_kv;
        u32 addr_l_begin_mha;
        u32 addr_l_end_mha;
        u32 addr_kk;
        std::string flm_version;
        std::string exec_path;

        //vision specific
        std::string vision_model_weight;
        nlohmann::json _vision_config;
        std::string audio_model_weight;
        nlohmann::json _audio_config;
        bool is_vlm;
        bool is_audio;


        nlohmann::json _json_config;

        /// \brief from pretrained
        /// \param model_name the model name
        void from_pretrained(std::string model_name){
            // #define DEV_BUILD
            #ifdef DEV_BUILD
            #ifdef __WINDOWS__
                this->exec_path = "..\\..\\..\\";
            #else
                this->exec_path = "../../../";
            #endif
            #else
                this->exec_path = utils::find_xclbin_path();
            #endif
            this->model_path = model_name;
            this->model_name = std::filesystem::path(model_name).filename().string();
            std::ifstream file(this->model_path + "/config.json");
            if (!file.is_open()){
                std::cerr << "Failed to open file: " << model_name << std::endl;
                exit(1);
            }
            // read the json file as a string
            std::string json_str((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            this->_json_config = nlohmann::json::parse(json_str);
            JSON_GET(this->sliding_window, this->_json_config, "sliding_window", 0, u32);
            JSON_GET(this->sliding_window_pattern, this->_json_config, "sliding_window_pattern", 0, u32);
            JSON_GET(this->model_type, this->_json_config, "model_type", "", std::string);
            JSON_GET(this->head_dim, this->_json_config, "head_dim", 0, u32);
            JSON_GET(this->hidden_size, this->_json_config, "hidden_size", 0, u32);
            JSON_GET(this->hidden_act, this->_json_config, "hidden_act", "", std::string);
            JSON_GET(this->intermediate_size, this->_json_config, "intermediate_size", 0, u32);
            JSON_GET(this->num_attention_heads, this->_json_config, "num_attention_heads", 0, u32);
            JSON_GET(this->linear_conv_kernel_dim, this->_json_config, "linear_conv_kernel_dim", 0, u32);
            JSON_GET(this->linear_key_head_dim, this->_json_config, "linear_key_head_dim", 0, u32);
            JSON_GET(this->linear_num_key_heads, this->_json_config, "linear_num_key_heads", 0, u32);
            JSON_GET(this->linear_num_value_heads, this->_json_config, "linear_num_value_heads", 0, u32);
            JSON_GET(this->linear_value_head_dim, this->_json_config, "linear_value_head_dim", 0, u32);

            JSON_GET(this->num_hidden_layers, this->_json_config, "num_hidden_layers", 0, u32);
            JSON_GET(this->num_key_value_heads, this->_json_config, "num_key_value_heads", 0, u32);
            JSON_GET(this->pretraining_tp, this->_json_config, "pretraining_tp", 0, u32);
            JSON_GET(this->rms_norm_eps, this->_json_config, "rms_norm_eps", 0.0, f32);
            JSON_GET(this->rope_theta, this->_json_config, "rope_theta", 0.0, f32);
            JSON_GET(this->vocab_size, this->_json_config, "vocab_size", 0, u32);
            JSON_GET(this->addr_qk, this->_json_config, "addr_qk", 0, u32);
            JSON_GET(this->addr_kv, this->_json_config, "addr_kv", 0, u32);
            JSON_GET(this->addr_l_begin_mha, this->_json_config, "addr_l_begin_mha", 0, u32);
            JSON_GET(this->addr_l_end_mha, this->_json_config, "addr_l_end_mha", 0, u32);
            JSON_GET(this->addr_kk, this->_json_config, "addr_kk", 0, u32);


            // config for vision
            {
                JSON_GET(this->vision_model_weight, this->_json_config, "vision_model_weight", "", std::string);
                JSON_GET(this->_vision_config, this->_json_config, "vision_config", nlohmann::json::object(), nlohmann::json);
                JSON_GET(this->_audio_config, this->_json_config, "audio_config", nlohmann::json::object(), nlohmann::json);
                JSON_GET(this->audio_model_weight, this->_json_config, "audio_model_weight", "", std::string);

            }
            this->is_vlm = this->vision_model_weight != "";
            this->is_audio = this->audio_model_weight != "";


            JSON_GET(this->flm_version, this->_json_config, "flm_version", "0.0.0", std::string);
            assert(this->vocab_size > 0);
            assert(this->hidden_size > 0);
            assert(this->intermediate_size > 0);
            assert(this->num_attention_heads > 0);
            assert(this->num_hidden_layers > 0);
            assert(this->num_key_value_heads > 0);
            this->vision_model_weight = this->model_path + "/" + this->vision_model_weight;
            this->audio_model_weight = this->model_path + "/" + this->audio_model_weight;
        }
        std::string _str(){
            std::stringstream ss;
            ss << "  Model: "  << std::endl;
            ss << "    model_name:             " << this->model_name << std::endl;
            ss << "    compatible_flm_version: >= " << this->flm_version << std::endl;
            ss << "    head_dim:               " << this->head_dim << std::endl;
            ss << "    hidden_size:            " << this->hidden_size << std::endl;
            if (this->hidden_act != ""){
                ss << "    hidden_act:             " << this->hidden_act << std::endl;
            }
            ss << "    intermediate_size:      " << this->intermediate_size << std::endl;
            ss << "    num_attention_heads:    " << this->num_attention_heads << std::endl;
            ss << "    num_hidden_layers:      " << this->num_hidden_layers << std::endl;
            ss << "    num_key_value_heads:    " << this->num_key_value_heads << std::endl;
            ss << "    pretraining_tp:         " << this->pretraining_tp << std::endl;
            ss << "    rms_norm_eps:           " << this->rms_norm_eps << std::endl;
            if (this->sliding_window > 0){
                ss << "    sliding_window:         " << this->sliding_window << std::endl;
                ss << "    sliding_window_pattern: " << this->sliding_window_pattern << std::endl;
            }

            if(this->is_vlm){
                ss << "  Vision: "  << std::endl;
                ss << "    vision_model_weight:    " << this->vision_model_weight << std::endl;
            }
            return ss.str();
        }
        LM_Config(){}
};

class Whisper_Config : public LM_Config{
public:
    /// \brief from pretrained
    /// \param model_name the model name
    void from_pretrained(std::string model_name){
        this->model_path = model_name;
        #ifdef DEV_BUILD
        #ifdef __WINDOWS__
            this->exec_path = "..\\..\\..\\";
        #else
            this->exec_path = "../../../";
        #endif
        #else
            this->exec_path = utils::find_xclbin_path();
        #endif
        this->model_name = std::filesystem::path(model_name).filename().string();
        std::ifstream file(model_name + "/config.json");
        if (!file.is_open()){
            std::cerr << "Failed to open file: " << model_name << std::endl;
            exit(1);
        }
        // read the json file as a string
        std::string json_str((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        this->_json_config = nlohmann::json::parse(json_str);
        JSON_GET(this->sliding_window, this->_json_config, "sliding_window", 0, u32);
        JSON_GET(this->sliding_window_pattern, this->_json_config, "sliding_window_pattern", 0, u32);
        JSON_GET(this->model_type, this->_json_config, "model_type", "", std::string);
        JSON_GET(this->head_dim, this->_json_config, "head_dim", 0, u32);
        JSON_GET(this->hidden_size, this->_json_config, "d_model", 0, u32);
        JSON_GET(this->hidden_act, this->_json_config, "hidden_act", "", std::string);
        JSON_GET(this->intermediate_size, this->_json_config, "decoder_ffn_dim", 0, u32);
        JSON_GET(this->num_attention_heads, this->_json_config, "num_attention_heads", 0, u32);
        JSON_GET(this->num_hidden_layers, this->_json_config, "num_hidden_layers", 0, u32);
        JSON_GET(this->num_key_value_heads, this->_json_config, "num_key_value_heads", 0, u32);
        JSON_GET(this->pretraining_tp, this->_json_config, "pretraining_tp", 0, u32);
        JSON_GET(this->rms_norm_eps, this->_json_config, "rms_norm_eps", 0.0, f32);
        JSON_GET(this->rope_theta, this->_json_config, "rope_theta", 0.0, f32);
        JSON_GET(this->vocab_size, this->_json_config, "vocab_size", 0, u32);
        JSON_GET(this->addr_qk, this->_json_config, "addr_qk", 0, u32);
        JSON_GET(this->addr_kv, this->_json_config, "addr_kv", 0, u32);
        JSON_GET(this->addr_l_begin_mha, this->_json_config, "addr_l_begin_mha", 0, u32);
        JSON_GET(this->addr_l_end_mha, this->_json_config, "addr_l_end_mha", 0, u32);
        JSON_GET(this->addr_kk, this->_json_config, "addr_kk", 0, u32);

        this->is_vlm = false;
        this->vocab_size = (this->vocab_size + 31) / 32 * 32;

        JSON_GET(this->flm_version, this->_json_config, "flm_version", "0.0.0", std::string);

        this->vision_model_weight = this->model_path + "/" + this->vision_model_weight;
    }
    std::string _str(){
        std::stringstream ss;
        ss << "  Model: "  << std::endl;
        ss << "    model_name:             " << this->model_name << std::endl;
        ss << "    compatible_flm_version: >= " << this->flm_version << std::endl;
        ss << "    head_dim:               " << this->head_dim << std::endl;
        ss << "    hidden_size:            " << this->hidden_size << std::endl;
        if (this->hidden_act != ""){
            ss << "    hidden_act:             " << this->hidden_act << std::endl;
        }
        ss << "    intermediate_size:      " << this->intermediate_size << std::endl;
        ss << "    num_attention_heads:    " << this->num_attention_heads << std::endl;
        ss << "    num_hidden_layers:      " << this->num_hidden_layers << std::endl;
        ss << "    num_key_value_heads:    " << this->num_key_value_heads << std::endl;
        ss << "    pretraining_tp:         " << this->pretraining_tp << std::endl;
        ss << "    rms_norm_eps:           " << this->rms_norm_eps << std::endl;
        if (this->sliding_window > 0){
            ss << "    sliding_window:         " << this->sliding_window << std::endl;
            ss << "    sliding_window_pattern: " << this->sliding_window_pattern << std::endl;
        }
        return ss.str();
    }
    Whisper_Config(){}
};

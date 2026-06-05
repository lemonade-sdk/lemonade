/*!
 *  Copyright (c) 2023 by Contributors
 * \file main.cpp
 * \brief Main entry point for the FLM application
 * \author FastFlowLM Team
 * \date 2025-06-24
 *  \version 0.9.24
 */
#pragma once
#include "utils/utils.hpp"
#include <nlohmann/json.hpp>
#include "model_list.hpp"
#include "wstream_buf.hpp"
#include "model_downloader.hpp"
#include "cli_wide.hpp"
#include "program_args.hpp"
#include "image/image_reader.hpp"
#ifndef FASTFLOWLM_LINUX_LIMITED_MODELS
#include "whisper/modeling_whisper.hpp"
#include "AutoEmbeddingModel/all_embedding_model.hpp"
#endif
#include <codecvt>
#include <vector>


#include "AutoModel/all_models.hpp"

using json = nlohmann::ordered_json;

/// \brief Command types
typedef enum {
    CMD_SET,
    CMD_SHOW,
    CMD_LOAD,
    CMD_SAVE,
    CMD_CLEAR,
    CMD_BYE,
    CMD_PULL,
    CMD_HELP,
    CMD_HELP_SHOTCUT,
    CMD_STATUS
} runner_cmd_t;

/// \brief Runner class
class Runner {
    public:
        Runner(model_list& supported_models, ModelDownloader& downloader, program_args_t& args);
        void run();
    private:
        std::string tag;
        int prefill_chunk_len;
        bool asr;
        bool asr_supported;
        bool embed;
        model_list supported_models;
        ModelDownloader& downloader;
        std::unique_ptr<AutoModel> auto_chat_engine;
#ifndef FASTFLOWLM_LINUX_LIMITED_MODELS
        std::unique_ptr<Whisper> whisper_engine;
        std::unique_ptr<AutoEmbeddingModel> auto_embedding_engine;
#endif
        int generate_limit;
        int ctx_length;
        std::string system_prompt;
        bool preemption;
        int img_pre_resize;
        // CLI instance for interactive input
        CLIWide cli;
        xrt::device npu_device_inst;

        /// \brief Command functions
        void cmd_set(std::vector<std::string>& input_list);
        void cmd_show(std::vector<std::string>& input_list);
        void cmd_load(std::vector<std::string>& input_list);
        void cmd_save(std::vector<std::string>& input_list);
        void cmd_clear(std::vector<std::string>& input_list);
        void cmd_help(std::vector<std::string>& input_list);
        void cmd_help_shotcut(std::vector<std::string>& input_list);
        void cmd_status(std::vector<std::string>& input_list);
};

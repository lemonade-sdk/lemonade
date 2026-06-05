/// \file automodel.hpp
/// \brief automodel class
/// \author FastFlowLM Team
/// \date 2025-09-01
/// \version 0.9.24
/// \note This is a header file for the auto_model class
#pragma once

#include <ctime>
#include <iomanip>
#include <sstream>
#include <memory>
#include <vector>
#include <iostream>
#include <string>
#include <type_traits>
#include <any>
#include "typedef.hpp"
#include "causal_lm.hpp"
#include "lm_config.hpp"
#include "models/llama/llama_npu.hpp"
#include "models/qwen2/qwen2_npu.hpp"
#include "models/qwen3/qwen3_npu.hpp"
#include "models/qwen2vl/qwen2vl_npu.hpp"
#include "models/qwen3vl/qwen3vl_npu.hpp"
#include "models/qwen3_5vl/qwen3_5vl_npu.hpp"
#include "models/gemma/gemma_npu.hpp"
#include "models/gemma_text/gemma_text_npu.hpp"
#include "models/gemma4e/gemma4e_npu.hpp"
#include "models/lfm2/lfm2_npu.hpp"
#include "models/phi4/phi4_npu.hpp"
#include "models/gpt_oss/gpt_oss_npu.hpp"
#include "models/nanbeige/nanbeige_npu.hpp"
#include "tokenizer/tokenizer.hpp"
#include "modules/sampler.hpp"
#include "utils/utils.hpp"
#include "utils/profiler.hpp"
#include "tensor_utils/q4_npu_eXpress.hpp"
#include "npu_utils/npu_utils.hpp"
#include "minja/chat-template.hpp"
#include <nlohmann/json.hpp>

// Forward declaration
struct CancellationToken;

enum class StreamEventType {
	WAITING,
	CONTENT,
	REASONING,
	TOOL_DONE,
};

struct StreamResult {
	StreamEventType type;
	std::string content;
	std::string tool_id;
	std::string tool_name;
	std::string tool_args_str;
};

struct NonStreamResult {
	std::string content;
	std::string reasoning_content;
	std::string tool_name;
	std::string tool_args;
};

typedef enum {
    EOT_DETECTED,
    MAX_LENGTH_REACHED,
    ERROR_DETECTED,
	CANCEL_DETECTED,
	TOOL_DETECTED
} stop_reason_t;

inline std::string stop_reason_to_string(stop_reason_t reason){
    switch (reason){
        case EOT_DETECTED:
            return "stop";
        case MAX_LENGTH_REACHED:
            return "length";
		case CANCEL_DETECTED:
			return "cancel";
        case ERROR_DETECTED:
            return "error";
		case TOOL_DETECTED:
			return "tool_calls";
		default:
            return "UNKNOWN";
    }
}

struct chat_meta_info_t {
	int max_prefill_len;
    int prompt_tokens;
    int generated_tokens;
    uint64_t total_duration; // in nanoseconds
    uint64_t load_duration; // in nanoseconds
    uint64_t prefill_duration; // in nanoseconds
    uint64_t decoding_duration; // in nanoseconds
    stop_reason_t stop_reason;
	bool restore_allowed;

	chat_meta_info_t() : max_prefill_len(0), prompt_tokens(0), generated_tokens(0), total_duration(0), load_duration(0), prefill_duration(0), decoding_duration(0), stop_reason(EOT_DETECTED), restore_allowed(false) {}
};

typedef enum {
	FILE_NAME,
	BASE64_ENCODED,
	URL_LINK
} input_payload_type_t; // so far not used, leave for future use

typedef enum {
	chat_ml,
	harmony,
	gemma4,
	unknown_template
} chat_template_type_t;

struct lm_uniform_input_t {
	std::string prompt;
	nlohmann::ordered_json messages;
	std::vector<std::string> images;
	std::vector<input_payload_type_t> image_payload_types;
	std::vector<std::string> audios;
	std::vector<input_payload_type_t> audio_payload_types;
	nlohmann::ordered_json tools;
};

using json = nlohmann::ordered_json;

class AutoModel {
protected:
	std::string model_path = "";
	std::unique_ptr<causal_lm> lm_engine = nullptr;
	std::unique_ptr<Tokenizer> tokenizer = nullptr;
	std::unique_ptr<Sampler> sampler = nullptr;
	std::unique_ptr<Q4NX> q4nx = nullptr;
	bool is_model_loaded = false;
	std::string current_model = "";
	std::vector<int> token_history;
	xrt::device* npu_device_inst = nullptr;
	std::unique_ptr<npu_xclbin_manager> npu = nullptr;
	bool enable_preemption = false;
    std::vector<int> checkpoint_his;


	uint32_t MAX_L = 0;
	int last_token = -1;
	uint32_t total_tokens = 0;
	std::unique_ptr<LM_Config> lm_config = nullptr;
	std::unique_ptr<minja::chat_template> chat_tmpl = nullptr;

	std::string bos_token;
    std::string eos_token;
    std::string boi_token;
    std::string eoi_token;
    bool has_bos_token;
    int bos_token_id;
    std::vector<int> eos_token_ids;

	std::string user_system_prompt = "";

    nlohmann::json extra_context;

	typedef enum {
		PREFILL_TIME,
		DECODING_TIME,
		SAMPLING_TIME,
		TKOEN_ENCODE_TIME,
		TKOEN_DECODE_TIME,
		TTFT_TIME,
		TOTAL_TIME,
		PROFILER_TYPE_NUM
	} profiler_type;
	std::vector<profiler> profiler_list;
	time_utils::time_with_unit last_prefill_time;

	std::string tool_name_;
	bool is_in_tool_block_ = false;
	std::string buffer_;
	StreamEventType current_mode_ = StreamEventType::CONTENT;
	bool waiting_for_header_ = true;



	void _shared_load_model(std::string model_path, json model_info, int default_context_length = -1, bool enable_preemption = false);
	nlohmann::json _shared_setup_tokenizer(std::string model_path);

	/// \brief Insert tokens into the model
	/// \param meta_info the meta information of the chat
	/// \param tokens the tokens to insert
	/// \param payload the payload, it shall not be used as this function is only used for chunkwised insertion, no image allowed
	/// \return true if the tokens were inserted successfully, false otherwise
	bool _shared_insert(chat_meta_info_t& meta_info, std::vector<int>& tokens, std::function<bool()> is_cancelled = [] { return false; }, void* payload = nullptr, int first_len_run = 0);
	buffer<bf16> _chunked_insert(chat_meta_info_t& meta_info, std::vector<int>& tokens, std::function<bool()> is_cancelled = [] { return false; }, void* payload = nullptr, int first_len_run = 0);
	std::string _shared_generate(chat_meta_info_t& meta_info, int length_limit, std::ostream& os, std::function<bool()> is_cancelled = [] { return false; });

	StreamResult _shared_think_tool_calling_pasrsed(const std::string content);

public:
	//************ Shared by all models *************/
	virtual ~AutoModel() = default;

	AutoModel(xrt::device* npu_device_inst, std::string current_model = "");

	void reset_parser() {
		buffer_.clear();
		is_in_tool_block_ = false;
		current_mode_ = StreamEventType::CONTENT;
	}

	/// \brief Clear the context
	void clear_context();

	/// \brief Get the current model
	/// \return the current model
	std::string get_current_model();

	/// \brief Get the current context length
	/// \return the current context length
	int get_current_context_length();

	/// \brief Set the sampler
	/// \param sampler_config the sampler config
	void set_sampler(sampler_config& sampler_config);

	/// \brief Set the max length
	/// \param MAX_L the max length
	void set_max_length(unsigned int MAX_L);

	/// \brief Get the max length
	/// \return the max length
	unsigned int get_max_length() const { return MAX_L; }

	/// \brief Show the model info
	/// \return the model info
	std::string show_model_info();

	/// \brief Show the profile
	/// \return the profile
	std::string show_profile();

	/// \brief Get the history
	/// \return the history
	std::pair<std::string, std::vector<int>> get_history();

	/// \brief Verbose
	void verbose();

	float get_ttft(){
    	time_utils::time_with_unit ttft_time = this->profiler_list[TTFT_TIME].get_total_time();
		time_utils::time_with_unit ttft_in_second = time_utils::cast_to_s(ttft_time);
		return ttft_in_second.first;
	}

	/// \brief Set the topk
	/// \param topk the topk
	void set_topk(int topk);

	/// \brief Set the topp
	/// \param topp the topp
	void set_topp(float topp);

	/// \brief Set the minp
	/// \param topp the minp
	void set_minp(float minp);

	/// \brief Set the temperature
	/// \param temperature the temperature
	void set_temperature(float temperature);

	/// \brief Set the presencepenalty
	/// \param presence_penalty the presence penalty
	void set_presence_penalty(float presence_penalty);

	/// \brief Set the repetition penalty
	/// \param repetition_penalty the repetition penalty
	void set_repetition_penalty(float repetition_penalty);

	/// \brief Set the frequency penalty
	/// \param frequency_penalty the frequency penalty
	void set_frequency_penalty(float frequency_penalty);

	/// \brief Set the frequency penalty window
	/// \param frequency_penalty_window the frequency penalty window
	void set_frequency_penalty_window(int frequency_penalty_window);

	/// \brief Set the penalty window
	/// \param penalty_window the penalty window
	void set_penalty_window(int penalty_window);

	/// \brief Start the ttft timer
	/// \return the ttft timer
	void start_ttft_timer();

	/// \brief Stop the ttft timer
	/// \return the ttft timer
	void stop_ttft_timer();

	/// \brief Reset the total timer
	/// \return the total timer
	void reset_total_timer();

	/// \brief Start the total timer
	/// \return the total timer
	void start_total_timer();

	/// \brief Stop the total timer
	/// \return the total timer
	void stop_total_timer();


	inline bool is_normal_token(int token) {
		if (token == this->bos_token_id){
			return false;
		}
		else {
			for (auto& id : this->eos_token_ids){
				if (token == id){
					return false;
				}
			}
		}
		return true;
	}

	inline bool is_eos(int token) {
		return std::find(this->eos_token_ids.begin(), this->eos_token_ids.end(), token) != this->eos_token_ids.end();
	}

	/// \brief Prepare the benchmark
	/// \param text the text to benchmark
	/// \return a pair of the number of tokens and the text that produce 1000 tokens
	std::pair<int, std::string> prepare_benchmark(std::string& text){
		std::vector<int> tokens = this->tokenizer->encode(text);
		int num_tokens = tokens.size();
		std::string benchmark_text = text;
		if (num_tokens > 1000 - 32) {
			tokens.resize(1000 - 32); // considering that the chat template always add some tokens to it
			benchmark_text = this->tokenizer->decode(tokens);
		}
		return { num_tokens, benchmark_text };
	}

	//************ Unique for each model *************/

	virtual void load_model(std::string model_path, json model_info, int default_context_length = -1, bool enable_preemption = false) {}
	virtual std::string generate(chat_meta_info_t& meta_info, int length_limit, std::ostream& os, std::function<bool()> is_cancelled = [] { return false; }) = 0;
	virtual chat_template_type_t get_chat_template_type() {
		return chat_template_type_t::chat_ml;
	}
	/// \brief Insert the tokens
	/// \param tokens the tokens
	/// \param is_system_prompt the is system prompt
	/// \return true if the tokens are inserted successfully, false otherwise
	virtual bool insert(chat_meta_info_t& meta_info, lm_uniform_input_t& input, std::function<bool()> is_cancelled = [] { return false; }) = 0;

	/// \brief Generate the tokens with prompt
	virtual std::string generate_with_prompt(chat_meta_info_t& meta_info, lm_uniform_input_t& input, int length_limit, std::ostream& os = std::cout) = 0;

	/// \brief Configure a parameter with type-erased value
	/// \param parameter_name the name of the parameter
	/// \param value the value to set (can be any type)
	/// \return true if the parameter was configured successfully, false otherwise
	virtual bool configure_parameter(std::string parameter_name, const std::any& value) {
		if (parameter_name == "system_prompt") {
			try {
				this->user_system_prompt = std::any_cast<std::string>(value);
				this->extra_context["user_system_prompt"] = this->user_system_prompt;
				return true;
			} catch (const std::bad_any_cast&) {
				return false;
			}
		}
		return false;
	}

	/// \brief Convenience template wrapper for type-safe parameter configuration
	template<typename T>
	bool configure_parameter(std::string parameter_name, const T& value) {
		return configure_parameter(parameter_name, std::any(value));
	}

	virtual std::string apply_chat_template(nlohmann::ordered_json& messages, nlohmann::ordered_json tools = nlohmann::ordered_json::object()) = 0;

	virtual NonStreamResult parse_nstream_content(const std::string response_text) {
		NonStreamResult result;
		result.content = response_text;
		return result;
	}

	virtual StreamResult parse_stream_content(const std::string content) {
		//header_print("AUTOMODEL PARSING", content);

		StreamResult result;
		result.type = StreamEventType::CONTENT;
		result.content = content;

		return result;
	}

	virtual StreamResult parse_stream_content_final(const std::string content) {
		if (!content.empty()) {
			return parse_stream_content(content);
		}

		StreamResult result;
		result.type = StreamEventType::WAITING;
		return result;
	}
};

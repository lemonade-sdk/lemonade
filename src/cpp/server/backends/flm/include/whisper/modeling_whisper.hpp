/// \file modeling_whisper.hpp
/// \brief modeling_whisper class
/// \author FastFlowLM Team
/// \date 2025-10-17
/// \version 0.9.24
/// \note This is a header file for the modeling_whisper class

#pragma once
#include "base64.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include "whisper/whisper_npu.hpp"
#include <nlohmann/json.hpp>
#include "utils/profiler.hpp"
#include "lm_config.hpp"
#include "tensor_utils/q4_npu_eXpress.hpp"
// FFmpeg includes for image processing only
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <vector>
#include "typedef.hpp"
#include <complex>
#include <cmath>
#include <stdexcept>
#include <iostream>
#include <unordered_map>
#include "fftw3.h"
#include <immintrin.h>  // For AVX intrinsics
#include "metrices.hpp"
#include "tokenizer/tokenizer.hpp"
#include "modules/sampler.hpp"
#include "whisper/language_table.hpp"

class FFT400 {
public:
    static constexpr int N = 400;

    FFT400() {
        // Allocate input and output arrays for FFTW
        in = (float*)fftwf_malloc(N * sizeof(float));
        out = (fftwf_complex*)fftwf_malloc((N/2 + 1) * sizeof(fftwf_complex));

        // Create FFTW plan for real-to-complex FFT
        plan = fftwf_plan_dft_r2c_1d(N, in, out, FFTW_ESTIMATE);

        if (!plan) {
            throw std::runtime_error("Failed to create FFTW plan");
        }
    }

    ~FFT400() {
        if (plan) {
            fftwf_destroy_plan(plan);
        }
        if (in) {
            fftwf_free(in);
        }
        if (out) {
            fftwf_free(out);
        }
    }

    // Delete copy constructor and assignment operator to prevent issues with FFTW plans
    FFT400(const FFT400&) = delete;
    FFT400& operator=(const FFT400&) = delete;

    std::vector<float> compute_power(const std::vector<float>& input) const {
        if (input.size() != N) {
            throw std::runtime_error("FFT400 input must have 400 samples, got " + std::to_string(input.size()));
        }

        // Copy input data to FFTW input array
        for (int i = 0; i < N; ++i) {
            in[i] = input[i];
        }

        // Execute FFT
        fftwf_execute(plan);

        // Extract power spectrum (magnitude squared)
        std::vector<float> power(N / 2 + 1);
        for (int k = 0; k <= N / 2; ++k) {
            float real = out[k][0];
            float imag = out[k][1];
            power[k] = real * real + imag * imag;
        }

        return power;
    }

private:
    float* in;
    fftwf_complex* out;
    fftwf_plan plan;
};

/************              Whisper            **************/
class Whisper {
private:

    static constexpr int FS = 16000;
    static constexpr int WINDOW_LENGTH = 30; // seconds
    static constexpr int WINDOW_SAMPLES = FS * WINDOW_LENGTH;
    static constexpr int start_of_transcript = 50258;
    static constexpr int start_of_prev_token = 50362;
    static constexpr int no_time_stamp_token = 50364;
    static constexpr int translate_token = 50359;
    static constexpr int transcribe_token = 50360;
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
    std::vector<float> audio_buffer;
    buffer<bf16> mel_feature;
    float time_stamp;

    std::string model_path;
    std::unique_ptr<FFT400> fft_400;
    std::unique_ptr<whisper_npu> whisper_engine;
    std::unique_ptr<npu_xclbin_manager> npu;
	std::unique_ptr<Whisper_Config> lm_config = nullptr;
    xrt::device* device;

    bool enable_preemption;

	std::unique_ptr<Tokenizer> tokenizer = nullptr;
	std::unique_ptr<Sampler> sampler = nullptr;

	std::string bos_token;
    std::string eos_token;
    std::string boi_token;
    std::string eoi_token;
    bool has_bos_token;
    int bos_token_id;
    std::vector<int> eos_token_ids;
    std::vector<float> token_time_map;
    int token_time_map_offset;
    int total_time_stamps;


	std::string user_system_prompt = "";

    nlohmann::json extra_context;
    std::vector<float> _load_audio(std::string& audio_path);
    std::vector<float> _load_audio(std::vector<uint8_t>& audio_data);
    void _preprocess_audio(buffer<bf16>& mel_features, std::vector<float>& audio);

    inline float _S2T_(int   sample_idx ) { return (float)sample_idx / FS;}
    inline int   _T2S_(float time_second) { return int(time_second * FS);}


    void _build_time_map();
    float _get_time(int& token_id){
        return this->token_time_map[token_id - token_time_map_offset];
    }
    int _get_token_id(float& time_stamp) {
        int rounded_time = (int) time_stamp * 50;
        return this->token_time_map_offset + rounded_time;
    }
    bool _is_time_stemp(int& token){
        return (token >= token_time_map_offset) && (token < (total_time_stamps + token_time_map_offset));
    }

    inline bool _is_normal_token(int& token){
        return token < 50257;
    }

    int _sample_in_language(buffer<bf16>& logits);

    int _sample_in_time_stamp(buffer<bf16>& logits);

    inline bool _is_valid_utf8(const std::string& input) {
        size_t i = 0;
        while (i < input.size()) {
            unsigned char c = input[i];
            size_t width;

            // --- Validate UTF-8 codepoint ---
            if (c < 0x80) {
                // Single byte ASCII character
                width = 1;
            }
            else if ((c & 0xE0) == 0xC0) {
                // 2-byte character
                if (i + 1 >= input.size()) return false;
                // Check that second byte is a valid continuation byte
                if ((input[i+1] & 0xC0) != 0x80) return false;
                width = 2;
            }
            else if ((c & 0xF0) == 0xE0) {
                // 3-byte character
                if (i + 2 >= input.size()) return false;
                // Check that second and third bytes are valid continuation bytes
                if ((input[i+1] & 0xC0) != 0x80 || (input[i+2] & 0xC0) != 0x80) return false;
                width = 3;
            }
            else if ((c & 0xF8) == 0xF0) {
                // 4-byte character
                if (i + 3 >= input.size()) return false;
                // Check that second, third, and fourth bytes are valid continuation bytes
                if ((input[i+1] & 0xC0) != 0x80 || (input[i+2] & 0xC0) != 0x80 || (input[i+3] & 0xC0) != 0x80) return false;
                width = 4;
            }
            else {
                // Invalid UTF-8 byte
                return false;
            }

            i += width;
        }
        return true;
    }


    std::string _offset_time_stamp(std::string& time_stamp, float offset){
        float time;
        sscanf(time_stamp.c_str(), "<|%f|>", &time);
        time += offset;
        char buffer[128];
        sprintf(buffer, "<|%.2f|>", time);
        return std::string(buffer);
    }
public:
    typedef enum {
        e_translate = 0,
        e_transcribe = 1
    } whisper_task_type_t;

    Whisper(xrt::device* npu_device_inst);

    void load_model(std::string model_path, nlohmann::ordered_json model_inf, bool enable_preemption = false);
    //void toggle_enable_think() override;
    bool load_audio(std::string& audio_path);
    bool load_audio(std::vector<uint8_t>& audio_data);
    std::pair<std::string, std::string> generate(whisper_task_type_t task, bool enable_time_stamp, bool return_time_stamp, std::ostream& os);
    void setup_tokenizer(std::string model_path);
};

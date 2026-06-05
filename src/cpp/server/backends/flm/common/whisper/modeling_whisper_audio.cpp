/// \file modeling_whisper_audio.hpp
/// \brief modeling_whisper_audio class
/// \author FastFlowLM Team
/// \date 2025-10-17
/// \version 0.9.24
/// \note This is a source file for the modeling_whisper_audio class
#include "whisper/modeling_whisper.hpp"
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <cmath>
#include <complex>
#include <algorithm>
#include <limits>
#include <vector>
#include <complex>
#include <cmath>
#include <stdexcept>
#include <algorithm>
#include <limits>
#include "utils/utils.hpp"
#include "utils/debug_utils.hpp"
#include "buffer.hpp"
#include "tensor_2d.hpp"
#include <immintrin.h>  // For AVX intrinsics

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
}

std::vector<float> Whisper::_load_audio(std::string& filename) {
    AVFormatContext* format_ctx = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    SwrContext* swr_ctx = nullptr;
    std::vector<float> result_audio_data;

    const int TARGET_SAMPLE_RATE = 16000;

    // Suppress FFmpeg warnings and info messages
    av_log_set_level(AV_LOG_ERROR);

    try {
        // Open input file
        if (avformat_open_input(&format_ctx, filename.c_str(), nullptr, nullptr) < 0) {
            throw std::runtime_error("Could not open audio file: " + filename);
        }

        // Retrieve stream information
        if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
            throw std::runtime_error("Could not find stream information");
        }

        // Find the audio stream
        int audio_stream_index = -1;
        for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
            if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audio_stream_index = i;
                break;
            }
        }

        if (audio_stream_index == -1) {
            throw std::runtime_error("Could not find audio stream");
        }

        AVStream* audio_stream = format_ctx->streams[audio_stream_index];
        AVCodecParameters* codec_params = audio_stream->codecpar;

        // Find decoder
        const AVCodec* codec = avcodec_find_decoder(codec_params->codec_id);
        if (!codec) {
            throw std::runtime_error("Unsupported codec");
        }

        // Allocate codec context
        codec_ctx = avcodec_alloc_context3(codec);
        if (!codec_ctx) {
            throw std::runtime_error("Could not allocate codec context");
        }

        // Copy codec parameters to codec context
        if (avcodec_parameters_to_context(codec_ctx, codec_params) < 0) {
            throw std::runtime_error("Could not copy codec parameters to codec context");
        }

        // Open codec
        if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
            throw std::runtime_error("Could not open codec");
        }

        // Get input sample rate and channel layout
        int input_sample_rate = codec_ctx->sample_rate;
        AVChannelLayout input_ch_layout = codec_ctx->ch_layout;
        AVSampleFormat input_sample_fmt = codec_ctx->sample_fmt;

        // Always setup resampler to convert to mono 16kHz s16le (like FFmpeg command)
        swr_ctx = swr_alloc();
        if (!swr_ctx) {
            throw std::runtime_error("Could not allocate resampler context");
        }

        // Setup output channel layout (mono)
        AVChannelLayout out_ch_layout = AV_CHANNEL_LAYOUT_MONO;

        // Set resampler options to match FFmpeg command: -f s16le -ac 1 -ar 16000
        av_opt_set_chlayout(swr_ctx, "in_chlayout", &input_ch_layout, 0);
        av_opt_set_int(swr_ctx, "in_sample_rate", input_sample_rate, 0);
        av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", input_sample_fmt, 0);

        av_opt_set_chlayout(swr_ctx, "out_chlayout", &out_ch_layout, 0);
        av_opt_set_int(swr_ctx, "out_sample_rate", TARGET_SAMPLE_RATE, 0);
        av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0); // s16le format

        // Initialize resampler
        if (swr_init(swr_ctx) < 0) {
            throw std::runtime_error("Could not initialize resampler");
        }

        // Read frames and decode
        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();

        if (!packet || !frame) {
            av_packet_free(&packet);
            av_frame_free(&frame);
            throw std::runtime_error("Could not allocate packet or frame");
        }

        while (av_read_frame(format_ctx, packet) >= 0) {
            if (packet->stream_index == audio_stream_index) {
                // Send packet to decoder
                if (avcodec_send_packet(codec_ctx, packet) >= 0) {
                    // Receive decoded frames
                    while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
                        // Calculate output samples
                        int out_samples = av_rescale_rnd(
                            swr_get_delay(swr_ctx, input_sample_rate) + frame->nb_samples,
                            TARGET_SAMPLE_RATE,
                            input_sample_rate,
                            AV_ROUND_UP
                        );

                        // Allocate output buffer for s16le format
                        uint8_t* output_buffer = nullptr;
                        int out_linesize;
                        av_samples_alloc(&output_buffer, &out_linesize, 1, out_samples,
                                       AV_SAMPLE_FMT_S16, 0);

                        // Resample to s16le format
                        int converted_samples = swr_convert(
                            swr_ctx,
                            &output_buffer,
                            out_samples,
                            (const uint8_t**)frame->data,
                            frame->nb_samples
                        );

                        if (converted_samples > 0) {
                            // Convert s16le to float32 (normalize to [-1.0, 1.0])
                            int16_t* s16_data = reinterpret_cast<int16_t*>(output_buffer);
                            for (int i = 0; i < converted_samples; i++) {
                                result_audio_data.push_back(s16_data[i] / 32768.0f);
                            }
                        }

                        av_freep(&output_buffer);
                    }
                }
            }
            av_packet_unref(packet);
        }

        // Flush decoder
        avcodec_send_packet(codec_ctx, nullptr);
        while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
            int out_samples = av_rescale_rnd(
                swr_get_delay(swr_ctx, input_sample_rate) + frame->nb_samples,
                TARGET_SAMPLE_RATE,
                input_sample_rate,
                AV_ROUND_UP
            );

            uint8_t* output_buffer = nullptr;
            int out_linesize;
            av_samples_alloc(&output_buffer, &out_linesize, 1, out_samples,
                           AV_SAMPLE_FMT_S16, 0);

            int converted_samples = swr_convert(
                swr_ctx,
                &output_buffer,
                out_samples,
                (const uint8_t**)frame->data,
                frame->nb_samples
            );

            if (converted_samples > 0) {
                // Convert s16le to float32 (normalize to [-1.0, 1.0])
                int16_t* s16_data = reinterpret_cast<int16_t*>(output_buffer);
                for (int i = 0; i < converted_samples; i++) {
                    result_audio_data.push_back(s16_data[i] / 32768.0f);
                }
            }

            av_freep(&output_buffer);
        }

        // Flush resampler
        if (swr_ctx) {
            int out_samples = av_rescale_rnd(swr_get_delay(swr_ctx, input_sample_rate),
                                            TARGET_SAMPLE_RATE, input_sample_rate, AV_ROUND_UP);
            if (out_samples > 0) {
                uint8_t* output_buffer = nullptr;
                int out_linesize;
                av_samples_alloc(&output_buffer, &out_linesize, 1, out_samples,
                               AV_SAMPLE_FMT_S16, 0);

                int converted_samples = swr_convert(swr_ctx, &output_buffer, out_samples,
                                                   nullptr, 0);

                if (converted_samples > 0) {
                    // Convert s16le to float32 (normalize to [-1.0, 1.0])
                    int16_t* s16_data = reinterpret_cast<int16_t*>(output_buffer);
                    for (int i = 0; i < converted_samples; i++) {
                        result_audio_data.push_back(s16_data[i] / 32768.0f);
                    }
                }

                av_freep(&output_buffer);
            }
        }

        // Cleanup
        av_packet_free(&packet);
        av_frame_free(&frame);

        if (swr_ctx) {
            swr_free(&swr_ctx);
        }
        if (codec_ctx) {
            avcodec_free_context(&codec_ctx);
        }
        if (format_ctx) {
            avformat_close_input(&format_ctx);
        }

        return result_audio_data;

    } catch (...) {
        // Cleanup on error
        if (swr_ctx) {
            swr_free(&swr_ctx);
        }
        if (codec_ctx) {
            avcodec_free_context(&codec_ctx);
        }
        if (format_ctx) {
            avformat_close_input(&format_ctx);
        }
        throw;
    }
}



std::vector<float> Whisper::_load_audio(std::vector<uint8_t>& audio_data) {
    AVFormatContext* format_ctx = nullptr;
    AVCodecContext* codec_ctx = nullptr;
    SwrContext* swr_ctx = nullptr;
    std::vector<float> result_audio_data;

    const int TARGET_SAMPLE_RATE = 16000;

    // Suppress FFmpeg warnings and info messages
    av_log_set_level(AV_LOG_ERROR);

    try {
        // Allocate buffer using FFmpeg's allocator so it can be safely freed
        uint8_t* io_buffer = (uint8_t*)av_malloc(audio_data.size());
        if (!io_buffer) {
            throw std::runtime_error("Could not allocate IO buffer for memory");
        }
        std::memcpy(io_buffer, audio_data.data(), audio_data.size());

        // Create a custom IO context for memory buffer
        AVIOContext* raw_io_ctx = avio_alloc_context(
            io_buffer,                  // buffer (now FFmpeg-managed)
            audio_data.size(),          // buffer size
            0,                          // write flag (0 = read-only)
            nullptr,                    // opaque (not needed)
            nullptr,                    // read_packet (not needed for read-only)
            nullptr,                    // write_packet (not needed for read-only)
            nullptr                     // seek (not needed for read-only)
        );

        if (!raw_io_ctx) {
            av_free(io_buffer);
            throw std::runtime_error("Could not allocate IO context for memory buffer");
        }

        // Create shared_ptr with custom deleter for FFmpeg-allocated memory
        std::shared_ptr<AVIOContext> io_ctx(raw_io_ctx, [](AVIOContext* ctx) {
            if (ctx) {
                // Free the buffer first, then the context
                av_free(ctx->buffer);
                avio_context_free(&ctx);
            }
        });

        // Allocate format context
        format_ctx = avformat_alloc_context();
        if (!format_ctx) {
            throw std::runtime_error("Could not allocate format context");
        }

        // Set the custom IO context
        format_ctx->pb = io_ctx.get();

        // Open input from memory buffer
        if (avformat_open_input(&format_ctx, nullptr, nullptr, nullptr) < 0) {
            throw std::runtime_error("Could not open audio data from memory buffer");
        }

        // Retrieve stream information
        if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
            throw std::runtime_error("Could not find stream information");
        }

        // Find the audio stream
        int audio_stream_index = -1;
        for (unsigned int i = 0; i < format_ctx->nb_streams; i++) {
            if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audio_stream_index = i;
                break;
            }
        }

        if (audio_stream_index == -1) {
            throw std::runtime_error("Could not find audio stream");
        }

        AVStream* audio_stream = format_ctx->streams[audio_stream_index];
        AVCodecParameters* codec_params = audio_stream->codecpar;

        // Find decoder
        const AVCodec* codec = avcodec_find_decoder(codec_params->codec_id);
        if (!codec) {
            throw std::runtime_error("Unsupported codec");
        }

        // Allocate codec context
        codec_ctx = avcodec_alloc_context3(codec);
        if (!codec_ctx) {
            throw std::runtime_error("Could not allocate codec context");
        }

        // Copy codec parameters to codec context
        if (avcodec_parameters_to_context(codec_ctx, codec_params) < 0) {
            throw std::runtime_error("Could not copy codec parameters to codec context");
        }

        // Open codec
        if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
            throw std::runtime_error("Could not open codec");
        }

        // Get input sample rate and channel layout
        int input_sample_rate = codec_ctx->sample_rate;
        AVChannelLayout input_ch_layout = codec_ctx->ch_layout;
        AVSampleFormat input_sample_fmt = codec_ctx->sample_fmt;

        // Always setup resampler to convert to mono 16kHz s16le (like FFmpeg command)
        swr_ctx = swr_alloc();
        if (!swr_ctx) {
            throw std::runtime_error("Could not allocate resampler context");
        }

        // Setup output channel layout (mono)
        AVChannelLayout out_ch_layout = AV_CHANNEL_LAYOUT_MONO;

        // Set resampler options to match FFmpeg command: -f s16le -ac 1 -ar 16000
        av_opt_set_chlayout(swr_ctx, "in_chlayout", &input_ch_layout, 0);
        av_opt_set_int(swr_ctx, "in_sample_rate", input_sample_rate, 0);
        av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", input_sample_fmt, 0);

        av_opt_set_chlayout(swr_ctx, "out_chlayout", &out_ch_layout, 0);
        av_opt_set_int(swr_ctx, "out_sample_rate", TARGET_SAMPLE_RATE, 0);
        av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0); // s16le format

        // Initialize resampler
        if (swr_init(swr_ctx) < 0) {
            throw std::runtime_error("Could not initialize resampler");
        }

        // Read frames and decode
        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();

        if (!packet || !frame) {
            av_packet_free(&packet);
            av_frame_free(&frame);
            throw std::runtime_error("Could not allocate packet or frame");
        }

        while (av_read_frame(format_ctx, packet) >= 0) {
            if (packet->stream_index == audio_stream_index) {
                // Send packet to decoder
                if (avcodec_send_packet(codec_ctx, packet) >= 0) {
                    // Receive decoded frames
                    while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
                        // Calculate output samples
                        int out_samples = av_rescale_rnd(
                            swr_get_delay(swr_ctx, input_sample_rate) + frame->nb_samples,
                            TARGET_SAMPLE_RATE,
                            input_sample_rate,
                            AV_ROUND_UP
                        );

                        // Allocate output buffer for s16le format
                        uint8_t* output_buffer = nullptr;
                        int out_linesize;
                        av_samples_alloc(&output_buffer, &out_linesize, 1, out_samples,
                                       AV_SAMPLE_FMT_S16, 0);

                        // Resample to s16le format
                        int converted_samples = swr_convert(
                            swr_ctx,
                            &output_buffer,
                            out_samples,
                            (const uint8_t**)frame->data,
                            frame->nb_samples
                        );

                        if (converted_samples > 0) {
                            // Convert s16le to float32 (normalize to [-1.0, 1.0])
                            int16_t* s16_data = reinterpret_cast<int16_t*>(output_buffer);
                            for (int i = 0; i < converted_samples; i++) {
                                result_audio_data.push_back(s16_data[i] / 32768.0f);
                            }
                        }

                        av_freep(&output_buffer);
                    }
                }
            }
            av_packet_unref(packet);
        }

        // Flush decoder
        avcodec_send_packet(codec_ctx, nullptr);
        while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
            int out_samples = av_rescale_rnd(
                swr_get_delay(swr_ctx, input_sample_rate) + frame->nb_samples,
                TARGET_SAMPLE_RATE,
                input_sample_rate,
                AV_ROUND_UP
            );

            uint8_t* output_buffer = nullptr;
            int out_linesize;
            av_samples_alloc(&output_buffer, &out_linesize, 1, out_samples,
                           AV_SAMPLE_FMT_S16, 0);

            int converted_samples = swr_convert(
                swr_ctx,
                &output_buffer,
                out_samples,
                (const uint8_t**)frame->data,
                frame->nb_samples
            );

            if (converted_samples > 0) {
                // Convert s16le to float32 (normalize to [-1.0, 1.0])
                int16_t* s16_data = reinterpret_cast<int16_t*>(output_buffer);
                for (int i = 0; i < converted_samples; i++) {
                    result_audio_data.push_back(s16_data[i] / 32768.0f);
                }
            }

            av_freep(&output_buffer);
        }

        // Flush resampler
        if (swr_ctx) {
            int out_samples = av_rescale_rnd(swr_get_delay(swr_ctx, input_sample_rate),
                                            TARGET_SAMPLE_RATE, input_sample_rate, AV_ROUND_UP);
            if (out_samples > 0) {
                uint8_t* output_buffer = nullptr;
                int out_linesize;
                av_samples_alloc(&output_buffer, &out_linesize, 1, out_samples,
                               AV_SAMPLE_FMT_S16, 0);

                int converted_samples = swr_convert(swr_ctx, &output_buffer, out_samples,
                                                   nullptr, 0);

                if (converted_samples > 0) {
                    // Convert s16le to float32 (normalize to [-1.0, 1.0])
                    int16_t* s16_data = reinterpret_cast<int16_t*>(output_buffer);
                    for (int i = 0; i < converted_samples; i++) {
                        result_audio_data.push_back(s16_data[i] / 32768.0f);
                    }
                }

                av_freep(&output_buffer);
            }
        }

        // Cleanup
        av_packet_free(&packet);
        av_frame_free(&frame);

        if (swr_ctx) {
            swr_free(&swr_ctx);
        }
        if (codec_ctx) {
            avcodec_free_context(&codec_ctx);
        }
        if (format_ctx) {
            avformat_close_input(&format_ctx);
        }
        // Note: io_ctx will be automatically freed when format_ctx is closed
        // The shared_ptr will handle the cleanup when it goes out of scope

        return result_audio_data;

    } catch (...) {
        // Cleanup on error
        if (swr_ctx) {
            swr_free(&swr_ctx);
        }
        if (codec_ctx) {
            avcodec_free_context(&codec_ctx);
        }
        if (format_ctx) {
            avformat_close_input(&format_ctx);
        }
        // Note: io_ctx will be automatically freed when format_ctx is closed
        // The shared_ptr will handle the cleanup when it goes out of scope
        throw;
    }
}

static float hann_window[400] = {
    0.0000e+00, 6.1691e-05, 2.4673e-04, 5.5507e-04, 9.8664e-04, 1.5413e-03,
    2.2190e-03, 3.0195e-03, 3.9426e-03, 4.9882e-03, 6.1558e-03, 7.4453e-03,
    8.8564e-03, 1.0389e-02, 1.2042e-02, 1.3815e-02, 1.5708e-02, 1.7721e-02,
    1.9853e-02, 2.2103e-02, 2.4472e-02, 2.6957e-02, 2.9560e-02, 3.2278e-02,
    3.5112e-02, 3.8060e-02, 4.1123e-02, 4.4298e-02, 4.7586e-02, 5.0986e-02,
    5.4497e-02, 5.8117e-02, 6.1847e-02, 6.5684e-02, 6.9629e-02, 7.3680e-02,
    7.7836e-02, 8.2096e-02, 8.6460e-02, 9.0925e-02, 9.5491e-02, 1.0016e-01,
    1.0492e-01, 1.0978e-01, 1.1474e-01, 1.1980e-01, 1.2494e-01, 1.3018e-01,
    1.3552e-01, 1.4094e-01, 1.4645e-01, 1.5204e-01, 1.5773e-01, 1.6349e-01,
    1.6934e-01, 1.7528e-01, 1.8129e-01, 1.8738e-01, 1.9355e-01, 1.9979e-01,
    2.0611e-01, 2.1250e-01, 2.1896e-01, 2.2549e-01, 2.3209e-01, 2.3875e-01,
    2.4548e-01, 2.5227e-01, 2.5912e-01, 2.6604e-01, 2.7300e-01, 2.8003e-01,
    2.8711e-01, 2.9424e-01, 3.0143e-01, 3.0866e-01, 3.1594e-01, 3.2326e-01,
    3.3063e-01, 3.3804e-01, 3.4549e-01, 3.5298e-01, 3.6050e-01, 3.6806e-01,
    3.7566e-01, 3.8328e-01, 3.9093e-01, 3.9861e-01, 4.0631e-01, 4.1404e-01,
    4.2178e-01, 4.2955e-01, 4.3733e-01, 4.4513e-01, 4.5295e-01, 4.6077e-01,
    4.6860e-01, 4.7645e-01, 4.8429e-01, 4.9215e-01, 5.0000e-01, 5.0785e-01,
    5.1571e-01, 5.2355e-01, 5.3140e-01, 5.3923e-01, 5.4705e-01, 5.5487e-01,
    5.6267e-01, 5.7045e-01, 5.7822e-01, 5.8596e-01, 5.9369e-01, 6.0139e-01,
    6.0907e-01, 6.1672e-01, 6.2435e-01, 6.3194e-01, 6.3950e-01, 6.4702e-01,
    6.5451e-01, 6.6196e-01, 6.6937e-01, 6.7674e-01, 6.8406e-01, 6.9134e-01,
    6.9857e-01, 7.0576e-01, 7.1289e-01, 7.1997e-01, 7.2700e-01, 7.3396e-01,
    7.4088e-01, 7.4773e-01, 7.5452e-01, 7.6125e-01, 7.6791e-01, 7.7451e-01,
    7.8104e-01, 7.8750e-01, 7.9389e-01, 8.0021e-01, 8.0645e-01, 8.1262e-01,
    8.1871e-01, 8.2472e-01, 8.3066e-01, 8.3651e-01, 8.4227e-01, 8.4796e-01,
    8.5355e-01, 8.5906e-01, 8.6448e-01, 8.6982e-01, 8.7506e-01, 8.8020e-01,
    8.8526e-01, 8.9022e-01, 8.9508e-01, 8.9984e-01, 9.0451e-01, 9.0907e-01,
    9.1354e-01, 9.1790e-01, 9.2216e-01, 9.2632e-01, 9.3037e-01, 9.3432e-01,
    9.3815e-01, 9.4188e-01, 9.4550e-01, 9.4901e-01, 9.5241e-01, 9.5570e-01,
    9.5888e-01, 9.6194e-01, 9.6489e-01, 9.6772e-01, 9.7044e-01, 9.7304e-01,
    9.7553e-01, 9.7790e-01, 9.8015e-01, 9.8228e-01, 9.8429e-01, 9.8618e-01,
    9.8796e-01, 9.8961e-01, 9.9114e-01, 9.9255e-01, 9.9384e-01, 9.9501e-01,
    9.9606e-01, 9.9698e-01, 9.9778e-01, 9.9846e-01, 9.9901e-01, 9.9944e-01,
    9.9975e-01, 9.9994e-01, 1.0000e+00, 9.9994e-01, 9.9975e-01, 9.9944e-01,
    9.9901e-01, 9.9846e-01, 9.9778e-01, 9.9698e-01, 9.9606e-01, 9.9501e-01,
    9.9384e-01, 9.9255e-01, 9.9114e-01, 9.8961e-01, 9.8796e-01, 9.8618e-01,
    9.8429e-01, 9.8228e-01, 9.8015e-01, 9.7790e-01, 9.7553e-01, 9.7304e-01,
    9.7044e-01, 9.6772e-01, 9.6489e-01, 9.6194e-01, 9.5888e-01, 9.5570e-01,
    9.5241e-01, 9.4901e-01, 9.4550e-01, 9.4188e-01, 9.3815e-01, 9.3432e-01,
    9.3037e-01, 9.2632e-01, 9.2216e-01, 9.1790e-01, 9.1354e-01, 9.0907e-01,
    9.0451e-01, 8.9984e-01, 8.9508e-01, 8.9022e-01, 8.8526e-01, 8.8020e-01,
    8.7506e-01, 8.6982e-01, 8.6448e-01, 8.5906e-01, 8.5355e-01, 8.4796e-01,
    8.4227e-01, 8.3651e-01, 8.3066e-01, 8.2472e-01, 8.1871e-01, 8.1262e-01,
    8.0645e-01, 8.0021e-01, 7.9389e-01, 7.8750e-01, 7.8104e-01, 7.7451e-01,
    7.6791e-01, 7.6125e-01, 7.5452e-01, 7.4773e-01, 7.4088e-01, 7.3396e-01,
    7.2700e-01, 7.1997e-01, 7.1289e-01, 7.0576e-01, 6.9857e-01, 6.9134e-01,
    6.8406e-01, 6.7674e-01, 6.6937e-01, 6.6196e-01, 6.5451e-01, 6.4702e-01,
    6.3950e-01, 6.3194e-01, 6.2434e-01, 6.1672e-01, 6.0907e-01, 6.0139e-01,
    5.9369e-01, 5.8596e-01, 5.7822e-01, 5.7045e-01, 5.6267e-01, 5.5487e-01,
    5.4705e-01, 5.3923e-01, 5.3140e-01, 5.2355e-01, 5.1571e-01, 5.0785e-01,
    5.0000e-01, 4.9215e-01, 4.8429e-01, 4.7645e-01, 4.6860e-01, 4.6077e-01,
    4.5295e-01, 4.4513e-01, 4.3733e-01, 4.2955e-01, 4.2178e-01, 4.1404e-01,
    4.0631e-01, 3.9861e-01, 3.9093e-01, 3.8328e-01, 3.7565e-01, 3.6806e-01,
    3.6050e-01, 3.5298e-01, 3.4549e-01, 3.3804e-01, 3.3063e-01, 3.2326e-01,
    3.1594e-01, 3.0866e-01, 3.0143e-01, 2.9424e-01, 2.8711e-01, 2.8003e-01,
    2.7300e-01, 2.6604e-01, 2.5912e-01, 2.5227e-01, 2.4548e-01, 2.3875e-01,
    2.3209e-01, 2.2549e-01, 2.1896e-01, 2.1250e-01, 2.0611e-01, 1.9979e-01,
    1.9355e-01, 1.8738e-01, 1.8129e-01, 1.7528e-01, 1.6934e-01, 1.6349e-01,
    1.5773e-01, 1.5204e-01, 1.4645e-01, 1.4094e-01, 1.3552e-01, 1.3018e-01,
    1.2494e-01, 1.1980e-01, 1.1474e-01, 1.0978e-01, 1.0492e-01, 1.0016e-01,
    9.5491e-02, 9.0925e-02, 8.6460e-02, 8.2096e-02, 7.7836e-02, 7.3680e-02,
    6.9629e-02, 6.5684e-02, 6.1847e-02, 5.8117e-02, 5.4497e-02, 5.0986e-02,
    4.7586e-02, 4.4298e-02, 4.1123e-02, 3.8060e-02, 3.5112e-02, 3.2278e-02,
    2.9560e-02, 2.6957e-02, 2.4472e-02, 2.2103e-02, 1.9853e-02, 1.7721e-02,
    1.5708e-02, 1.3815e-02, 1.2042e-02, 1.0389e-02, 8.8564e-03, 7.4453e-03,
    6.1558e-03, 4.9882e-03, 3.9426e-03, 3.0195e-03, 2.2190e-03, 1.5413e-03,
    9.8664e-04, 5.5507e-04, 2.4673e-04, 6.1691e-05
};


static int mel_start_idx[128] = {
    1,   1,   2,   2,   3,   3,   4,   5,   5,   6,   6,   7,   8,   8,   9,   9,  10,  10,  11,  12,  12,  13,  13,  14,  15,  15,  16,  16,  17,  17,  18,  19,  19,  20,  20,  21,  22,  22,
         23,  23,  24,  24,  25,  26,  26,  27,  28,  28,  29,  30,  30,  31,  32,  32,  33,  34,  35,  36,  37,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  48,  49,  50,  51,  52,  54,  55,
         56,  58,  59,  60,  62,  63,  65,  66,  68,  70,  71,  73,  75,  77,  79,  80,  82,  84,  86,  89,  91,  93,  95,  98, 100, 102, 105, 107, 110, 113, 115, 118, 121, 124, 127, 130, 133, 136,
        140, 143, 147, 150, 154, 158, 161, 165, 169, 174, 178, 182, 187, 191
};

static int mel_count[128] = {1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 2, 1, 1, 2, 1, 1, 2, 1, 1, 2, 1, 1, 2, 2, 2, 2, 1, 1, 2, 2, 2, 2, 2,
    2, 2, 2, 3, 3, 2, 2, 2, 3, 3, 2, 3, 3, 2, 3, 3, 3, 3, 3, 4, 3, 3, 4, 4, 4, 3, 3, 4, 4, 5, 5, 4, 4, 5, 5, 4, 5, 5, 5, 6, 5, 5, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 8, 7, 7, 8, 9, 9, 8, 9, 9, 9, 9};

static float mel_filter_dense[] = {
    0.012373986653983593, 0.030392564833164215, 0.024747973307967186, 0.018018579110503197, 0.037121959030628204, 0.005644591990858316, 0.006729394197463989, 0.03603715822100639,
0.019103379920125008, 0.023663168773055077, 0.031477365642786026, 0.011289183981716633, 0.0010848019737750292, 0.04168175160884857, 0.013458788394927979, 0.029307762160897255,
0.025832774117588997, 0.016933776438236237, 0.038206759840250015, 0.004559790249913931, 0.007814195938408375, 0.03495235741138458, 0.020188182592391968, 0.022578367963433266,
0.032562170177698135, 0.010204383172094822, 0.0021696039475500584, 0.04059694707393646, 0.01454358920454979, 0.028222959488630295, 0.026917576789855957, 0.015848975628614426,
0.039291560649871826, 0.0034749882761389017, 0.008898998610675335, 0.03386755287647247, 0.021272985264658928, 0.021493567153811455, 0.033646970987319946, 0.009119580499827862,
0.003254405688494444, 0.03951214626431465, 0.01562839187681675, 0.027138158679008484, 0.028002377599477768, 0.014764172956347466, 0.040376365184783936, 0.0023806870449334383,
0.010202637873589993, 0.03161146119236946, 0.024547001346945763, 0.015329193323850632, 0.001665837480686605, 0.036729052662849426, 0.020097099244594574, 0.016931025311350822,
0.0029026553966104984, 0.032844990491867065, 0.023520048707723618, 0.011038944125175476, 0.010725829750299454, 0.022718291729688644, 0.03227872774004936, 0.00011626833293121308,
0.022853482514619827, 0.008563440293073654, 0.014979788102209568, 0.015513983555138111, 0.008514906279742718, 0.02110680378973484, 0.003326520323753357, 0.02547064796090126,
0.02735907956957817, 0.0006585361552424729, 0.02383812516927719, 0.0034435924608260393, 0.021224552765488625, 0.0053584217093884945, 0.019425557926297188, 0.006493247114121914,
0.018355421721935272, 0.006931380834430456, 0.017935046926140785, 0.0067496825940907, 0.018091518431901932, 0.006018991116434336, 0.018757672980427742, 0.004804528318345547,
0.019871728494763374, 0.0031662785913795233, 0.02137690968811512, 0.001253173453733325, 0.0011593446834012866, 0.02080361731350422, 0.004044868052005768, 0.017553631216287613,
0.007083200383931398, 0.014075386337935925, 0.010326550342142582, 0.010409214533865452, 0.013736963272094727, 0.006591876968741417, 0.017279881983995438, 0.0014680421445518732,
0.0026568188332021236, 0.01809193193912506, 0.005856557283550501, 0.013342779129743576, 0.010282675735652447, 0.00856800377368927, 0.01472230814397335, 0.001040398608893156,
0.003790855873376131, 0.01714678481221199, 0.006116092670708895, 0.011759290471673012, 0.011133937165141106, 0.006438578478991985, 0.01607806235551834, 0.004239172209054232,
0.0011998937698081136, 0.012756715528666973, 0.00965298991650343, 0.007069352548569441, 0.014940546825528145, 0.004190248437225819, 0.0015148338861763477, 0.012008999474346638,
0.009848233312368393, 0.006102240178734064, 0.01533857174217701, 0.005576768424361944, 0.00036827256553806365, 0.00989749375730753, 0.011353404261171818, 0.0020512230694293976,
0.003892971435561776, 0.012973522767424583, 0.00806631613522768, 0.006744932383298874, 0.013858746737241745, 0.005411905236542225, 0.0007422015769407153, 0.008987790904939175,
0.011378713883459568, 0.003329580882564187, 0.0028231353498995304, 0.010680492967367172, 0.00943340640515089, 0.0017632555682212114, 0.0043901861645281315, 0.011877589859068394,
0.007970058359205723, 0.0006610470009036362, 0.005494666751474142, 0.012629535980522633, 0.00693987961858511, 0.006184019148349762, 0.0129347313195467, 0.00629778765141964,
2.3252101527759805e-05, 0.006502066273242235, 0.0123266177251935, 0.006002165377140045, 0.00031548753031529486, 0.006489255465567112, 0.012041302397847176, 0.006014628801494837,
0.00029979555984027684, 0.006182880140841007, 0.012042728252708912, 0.006299811881035566, 0.0005568959750235081, 1.1204706424905453e-05, 0.005617291666567326, 0.011223378591239452,
0.0068251630291342735, 0.0013526449911296368, 0.004824100062251091, 0.010166232474148273, 0.007560755126178265, 0.002345903078094125, 0.003832357469946146, 0.008922962471842766,
0.00847910437732935, 0.003509786445647478, 0.0026687318459153175, 0.007519651670008898, 0.009555005468428135, 0.004819661378860474, 8.431751484749839e-05, 0.001357673667371273,
0.005980195011943579, 0.01060271542519331, 0.0062529849819839, 0.0017405991675332189, 0.004326442256569862, 0.008731318637728691, 0.007789165247231722, 0.003489238675683737,
0.0025783509481698275, 0.006775828544050455, 0.00940941646695137, 0.005311945918947458, 0.0012144759530201554, 0.0007541119121015072, 0.004753957036882639, 0.008753802627325058,
0.007192090153694153, 0.003287544008344412, 0.0026817969046533108, 0.0064933146350085735, 0.009114579297602177, 0.005393873900175095, 0.0016731682699173689, 0.0005739429616369307,
0.0042060003615915775, 0.007838058285415173, 0.007520230021327734, 0.003974708262830973, 0.00042918731924146414, 0.0019046447705477476, 0.005365691613405943, 0.008826738223433495,
0.0062760948203504086, 0.0028975096065551043, 0.0028988525737076998, 0.006196940783411264, 0.008566990494728088, 0.005347481928765774, 0.002127972897142172, 0.0004475022724363953,
0.0035903039388358593, 0.006733105983585119, 0.007770236115902662, 0.004702313803136349, 0.0016343912575393915, 0.0010153602343052626, 0.004010187461972237, 0.007005014456808567,
0.007234429940581322, 0.0043109566904604435, 0.0013874832075089216, 0.0013334885006770492, 0.004187308251857758, 0.007041127886623144, 0.0069318837486207485, 0.004146058112382889,
0.0013602323597297072, 0.0014287971425801516, 0.004148248583078384, 0.006867699790745974, 0.006837052758783102, 0.004182394593954086, 0.0015277357306331396, 0.0013261043932288885,
0.003917513880878687, 0.006508923601359129, 0.006926396861672401, 0.0043967291712760925, 0.0018670617137104273, 0.0010482777142897248, 0.0035176740493625402, 0.0059870705008506775,
0.007178240455687046, 0.004767679143697023, 0.002357117598876357, 0.0006163640646263957, 0.0029694922268390656, 0.005322620272636414, 0.007572650909423828, 0.005275587551295757,
0.0029785241931676865, 0.0006814609514549375, 4.9713995394995436e-05, 0.0022920481860637665, 0.004534382373094559, 0.006776716560125351, 0.0059024072252213955, 0.00371349835768342,
0.0015245892573148012, 0.0015028533525764942, 0.0036396104842424393, 0.005776367150247097, 0.0066315908916294575, 0.004545743577182293, 0.002459896495565772, 0.00037404923932626843,
0.0006179586052894592, 0.00265410915017128, 0.004690259229391813, 0.006726410239934921, 0.005460347048938274, 0.0034727093297988176, 0.0014850713778287172, 0.001592335756868124,
0.0035326166544109583, 0.005472897551953793, 0.0064436825923621655, 0.004549629986286163, 0.002655577613040805, 0.0007615251233801246, 0.00046749351895414293, 0.0023164190351963043,
0.004165344405919313, 0.0060142697766423225, 0.005678446963429451, 0.0038735736161470413, 0.002068700036033988, 0.0002638266596477479, 0.0010534910252317786, 0.002815362298861146,
0.0045772334560751915, 0.006339104846119881, 0.0051281568594276905, 0.0034082632046192884, 0.0016883700154721737, 0.0014335009036585689, 0.0031124167144298553, 0.00479133240878582,
0.00640943692997098, 0.004770522005856037, 0.0031316077802330256, 0.0014926930889487267, 2.9323589842533693e-05, 0.001629189937375486, 0.003229056019335985, 0.00482892245054245,
0.006146714556962252, 0.004584966227412224, 0.0030232176650315523, 0.0014614691026508808, 0.00013601698447018862, 0.001660555717535317, 0.003185094567015767, 0.004709633067250252,
0.006040723994374275, 0.0045525087043643, 0.003064292948693037, 0.001576077425852418, 8.786193211562932e-05, 9.328097075922415e-05, 0.001546038780361414, 0.002998796757310629,
0.004451554734259844, 0.0059043122455477715, 0.0046556610614061356, 0.003237516153603792, 0.0018193712458014488, 0.00040122633799910545, 0.0013026263331994414, 0.002686982974410057,
0.004071339499205351, 0.005455696024000645, 0.004878324922174215, 0.0035269514191895723, 0.0021755779162049294, 0.0008242045878432691, 0.0009459502762183547, 0.002265126211568713,
0.0035843022633343935, 0.0049034785479307175, 0.005205697845667601, 0.0039179520681500435, 0.00263020652346313, 0.001342460629530251, 5.471494296216406e-05, 0.0004903789376839995,
0.0017474433407187462, 0.003004507627338171, 0.004261571913957596, 0.005518636200577021, 0.004397072363644838, 0.0031699584797024727, 0.001942844595760107, 0.0007157306536100805,
0.0011469805613160133, 0.002344857668504119, 0.0035427347756922245, 0.004740611650049686, 0.0049519846215844154, 0.003782647429034114, 0.002613310469314456, 0.0014439737424254417,
0.0002746368118096143, 0.0004756950947921723, 0.0016171716852113605, 0.002758648479357362, 0.0039001251570880413, 0.005041601601988077, 0.004457120783627033, 0.003342840587720275,
0.0022285603918135166, 0.0011142801959067583
};
// Simple and reliable FFT for N=400


void Whisper::_preprocess_audio(buffer<bf16>& mel_features, std::vector<float>& audio) {
    const int SAMPLE_RATE = 16000;
    const int N_FFT = 400;
    const int HOP_LENGTH = 160;
    const int CHUNK_LENGTH = 30;
    const int n_mels = 128;
    const int N_SAMPLES = SAMPLE_RATE * CHUNK_LENGTH; // 480000

    // --- Pad/trim ---
    std::vector<float> x = audio;

    if ((int)x.size() > N_SAMPLES){
        x.resize(N_SAMPLES);
    }
    else if ((int)x.size() < N_SAMPLES){
        x.resize(N_SAMPLES, 0.0f);
    }

    // --- Reflection padding for proper STFT alignment ---
    // Pad so that t-th frame is centered at time t * hop_length
    int pad = N_FFT / 2;  // 200 samples

    std::vector<float> padded;
    padded.reserve(pad + x.size() + pad);

    // Left reflection padding: take x[0:pad] and reverse
    // This corresponds to x[0], x[1], ..., x[pad-1] in reverse order
    for (int i = pad - 1; i >= 0; --i) {
        padded.push_back(x[i]);
    }

    // Original signal
    padded.insert(padded.end(), x.begin(), x.end());

    // Right reflection padding: take x[size-pad:size] and reverse
    // This corresponds to x[size - 2], x[size-pad - 3], ..., x[size- pad] in reverse order

    for (int i = 0; i < pad - 1; i++) {
        padded.push_back(x[x.size() - 2 - i]);
    }


    // --- Correct number of frames ---
    // Standard STFT frame calculation: (padded_length - window_size) / hop_length + 1
    int n_frames = (static_cast<int>(padded.size()) - N_FFT) / HOP_LENGTH + 1; // = 3000
    int n_bins = N_FFT / 2 + 1;

    buffer<float> spec_buffer(n_bins * n_frames);
    tensor_2d<float> spec_tensor(spec_buffer, n_frames);
    std::vector<float> fft_buffer(N_FFT);

    for (int f = 0; f < n_frames; ++f) {
        // Frame f starts at position f * HOP_LENGTH in the padded signal
        // This ensures proper alignment with the reference implementation
        int start = f * HOP_LENGTH;

        // Bounds checking to prevent crashes
        if (start + N_FFT > static_cast<int>(padded.size())) {
            std::cerr << "Error: FFT frame " << f << " would exceed padded array bounds" << std::endl;
            std::cerr << "start: " << start << ", N_FFT: " << N_FFT << ", padded size: " << padded.size() << std::endl;
            throw std::runtime_error("FFT frame exceeds padded array bounds");
        }

        // AVX-optimized multiplication: process 8 floats at a time
        // N_FFT = 400, which is divisible by 8, so no remainder handling needed
        for (int i = 0; i < N_FFT; i += 8) {
            // Load 8 floats from padded array (starting at start + i)
            __m256 padded_vec = _mm256_loadu_ps(&padded[start + i]);

            // Load 8 floats from hann_window array
            __m256 hann_vec = _mm256_loadu_ps(&hann_window[i]);

            // Multiply the vectors
            __m256 result_vec = _mm256_mul_ps(padded_vec, hann_vec);

            // Store the result back to fft_buffer
            _mm256_storeu_ps(&fft_buffer[i], result_vec);
        }

        auto power = fft_400->compute_power(fft_buffer);

        for (int k = 0; k < n_bins; ++k) {
            spec_tensor[k][f] = power[k];
        }
    }

    // the spec_tensor is 201 x 3000
    // use 128 x 201 to multiply with 201 x 3000 to get 128 x 3000
    // --- Apply mel filters ---
    buffer<float> mel_spec(n_frames * n_mels);
    tensor_2d<float> mel_spec_tensor(mel_spec, n_frames);

    for (int f = 0; f < n_frames; ++f) {
        float *mel_val = mel_filter_dense;
        for (int m = 0; m < n_mels; ++m) {
            float s = 0.0f;
            for (int k = 0; k < mel_count[m]; ++k){
                int mel_idx = mel_start_idx[m] + k;
                s += spec_tensor[mel_idx][f] * (*mel_val++);
            }
            mel_spec_tensor[m][f] = s;
        }
    }

    // --- Log + normalize ---
    float global_max = -std::numeric_limits<float>::infinity();
    buffer<float> log_spec(n_frames * n_mels);
    tensor_2d<float> log_spec_tensor(log_spec, n_frames);
    for (int f = 0; f < n_frames; ++f) {
        for (int m = 0; m < n_mels; ++m) {
            float v = std::log10(std::max(mel_spec_tensor[m][f], 1e-10f));
            log_spec_tensor[m][f] = v;
            global_max = std::max(global_max, v);
        }
    }

    for (int f = 0; f < n_frames; ++f) {
        for (int m = 0; m < n_mels; ++m) {
            float v = std::max(log_spec_tensor[m][f], global_max - 8.0f);
            v = (v + 4.0f) / 4.0f;
            mel_features[m * n_frames + f] = (bf16)v;
        }
    }
}

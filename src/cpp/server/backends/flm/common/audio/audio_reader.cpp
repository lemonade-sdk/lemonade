/// \file audio_reader.cpp
/// \brief AudioReader implementation – decode and resample audio via FFmpeg
/// \author FastFlowLM Team

#include "audio/audio_reader.hpp"
#include <iostream>
#include <filesystem>
#include <cstring>
#include <mutex>
#include <cmath>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/avutil.h>
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
namespace {

static std::once_flag g_ffmpeg_init_flag;

static void init_ffmpeg_once() {
    std::call_once(g_ffmpeg_init_flag, []() {
        av_log_set_level(AV_LOG_ERROR);
    });
}

// Custom AVIO context for reading from memory
struct MemoryIOContext {
    const uint8_t* data;
    size_t size;
    size_t pos;
};

static int mem_read_packet(void* opaque, uint8_t* buf, int buf_size) {
    auto* ctx = static_cast<MemoryIOContext*>(opaque);
    size_t remaining = ctx->size - ctx->pos;
    if (remaining == 0) return AVERROR_EOF;
    size_t to_read = static_cast<size_t>(buf_size);
    if (to_read > remaining) to_read = remaining;
    std::memcpy(buf, ctx->data + ctx->pos, to_read);
    ctx->pos += to_read;
    return static_cast<int>(to_read);
}

static int64_t mem_seek(void* opaque, int64_t offset, int whence) {
    auto* ctx = static_cast<MemoryIOContext*>(opaque);
    int64_t new_pos = -1;
    switch (whence) {
        case SEEK_SET: new_pos = offset; break;
        case SEEK_CUR: new_pos = static_cast<int64_t>(ctx->pos) + offset; break;
        case SEEK_END: new_pos = static_cast<int64_t>(ctx->size) + offset; break;
        case AVSEEK_SIZE: return static_cast<int64_t>(ctx->size);
        default: return AVERROR(EINVAL);
    }
    if (new_pos < 0 || new_pos > static_cast<int64_t>(ctx->size)) return AVERROR(EINVAL);
    ctx->pos = static_cast<size_t>(new_pos);
    return new_pos;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// AudioReader
// ---------------------------------------------------------------------------
AudioReader::AudioReader() {
    init_ffmpeg_once();
}

AudioReader::~AudioReader() = default;

// ---------------------------------------------------------------------------
bool AudioReader::load_audio(const std::string& filename,
                             audio_data_t& out_audio,
                             int target_sample_rate,
                             MonoDownmixMode downmix)
{
    init_ffmpeg_once();

    if (!std::filesystem::exists(filename)) {
        std::cerr << "Error: audio file not found: " << filename << std::endl;
        return false;
    }

    AVFormatContext* format_ctx = nullptr;
    if (avformat_open_input(&format_ctx, filename.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "Error: could not open audio file: " << filename << std::endl;
        return false;
    }

    bool ok = decode_audio(format_ctx, out_audio, target_sample_rate, downmix);
    avformat_close_input(&format_ctx);
    return ok;
}

// ---------------------------------------------------------------------------
bool AudioReader::load_audio_from_memory(const uint8_t* data, size_t size,
                                         audio_data_t& out_audio,
                                         int target_sample_rate,
                                         MonoDownmixMode downmix)
{
    init_ffmpeg_once();

    if (!data || size == 0) {
        std::cerr << "Error: empty audio buffer" << std::endl;
        return false;
    }

    constexpr int kAVIOBufSize = 32768;
    auto* avio_buf = static_cast<uint8_t*>(av_malloc(kAVIOBufSize));
    if (!avio_buf) {
        std::cerr << "Error: could not allocate AVIO buffer" << std::endl;
        return false;
    }

    MemoryIOContext mem_ctx{data, size, 0};
    AVIOContext* avio_ctx = avio_alloc_context(avio_buf, kAVIOBufSize, 0,
                                               &mem_ctx, mem_read_packet, nullptr, mem_seek);
    if (!avio_ctx) {
        av_free(avio_buf);
        std::cerr << "Error: could not create AVIO context" << std::endl;
        return false;
    }

    AVFormatContext* format_ctx = avformat_alloc_context();
    if (!format_ctx) {
        avio_context_free(&avio_ctx);
        std::cerr << "Error: could not allocate AVFormatContext" << std::endl;
        return false;
    }
    format_ctx->pb = avio_ctx;

    if (avformat_open_input(&format_ctx, nullptr, nullptr, nullptr) < 0) {
        // avformat_open_input frees format_ctx on failure
        avio_context_free(&avio_ctx);
        std::cerr << "Error: could not open audio from memory buffer" << std::endl;
        return false;
    }

    bool ok = decode_audio(format_ctx, out_audio, target_sample_rate, downmix);
    avformat_close_input(&format_ctx);
    avio_context_free(&avio_ctx);
    return ok;
}

// ---------------------------------------------------------------------------
// Core decode + resample logic (shared by file and memory paths)
// ---------------------------------------------------------------------------
bool AudioReader::decode_audio(AVFormatContext* format_ctx,
                               audio_data_t& out_audio,
                               int target_sample_rate,
                               MonoDownmixMode downmix)
{
    if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
        std::cerr << "Error: could not find stream information" << std::endl;
        return false;
    }

    // Find the first audio stream
    int audio_stream_idx = -1;
    for (unsigned i = 0; i < format_ctx->nb_streams; ++i) {
        if (format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_idx = static_cast<int>(i);
            break;
        }
    }
    if (audio_stream_idx < 0) {
        std::cerr << "Error: no audio stream found" << std::endl;
        return false;
    }

    AVStream* audio_stream = format_ctx->streams[audio_stream_idx];
    AVCodecParameters* codec_params = audio_stream->codecpar;

    // Stash original metadata before decoding
    const int original_sample_rate = codec_params->sample_rate;
    const int original_channels    = codec_params->ch_layout.nb_channels;

    // Open decoder
    const AVCodec* codec = avcodec_find_decoder(codec_params->codec_id);
    if (!codec) {
        std::cerr << "Error: unsupported audio codec" << std::endl;
        return false;
    }

    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        std::cerr << "Error: could not allocate codec context" << std::endl;
        return false;
    }

    if (avcodec_parameters_to_context(codec_ctx, codec_params) < 0) {
        avcodec_free_context(&codec_ctx);
        std::cerr << "Error: could not copy codec parameters" << std::endl;
        return false;
    }

    if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
        avcodec_free_context(&codec_ctx);
        std::cerr << "Error: could not open audio codec" << std::endl;
        return false;
    }

    // Setup resampler: any input → mono float32 at target_sample_rate
    SwrContext* swr_ctx = swr_alloc();
    if (!swr_ctx) {
        avcodec_free_context(&codec_ctx);
        std::cerr << "Error: could not allocate resampler" << std::endl;
        return false;
    }

    av_opt_set_chlayout(swr_ctx, "in_chlayout",   &codec_ctx->ch_layout, 0);
    av_opt_set_int(swr_ctx,      "in_sample_rate",  codec_ctx->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", codec_ctx->sample_fmt, 0);

    // Determine output channel layout
    const bool do_downmix = (downmix != MonoDownmixMode::NONE) && (original_channels > 1);
    AVChannelLayout out_chlayout;
    if (do_downmix) {
        out_chlayout = AV_CHANNEL_LAYOUT_MONO;
    } else {
        out_chlayout = codec_ctx->ch_layout;
    }
    const int out_channels = out_chlayout.nb_channels;

    av_opt_set_chlayout(swr_ctx, "out_chlayout",   &out_chlayout, 0);
    av_opt_set_int(swr_ctx,      "out_sample_rate",  target_sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);

    // Override FFmpeg's default downmix matrix with custom coefficients
    // via swr_set_matrix(). Must be called before swr_init().
    if (do_downmix) {
        // matrix is out_channels x in_channels (row-major), stride = in_channels
        std::vector<double> matrix(original_channels);
        double coeff;
        switch (downmix) {
            case MonoDownmixMode::MEAN:
                // Simple average: 1/N per channel (matches librosa's to_mono)
                coeff = 1.0 / original_channels;
                break;
            case MonoDownmixMode::RMS:
                // Energy-preserving: 1/sqrt(N) per channel (FFmpeg default)
                coeff = 1.0 / std::sqrt(static_cast<double>(original_channels));
                break;
            default:
                coeff = 1.0 / original_channels;
                break;
        }
        for (int c = 0; c < original_channels; ++c) {
            matrix[c] = coeff;
        }
        swr_set_matrix(swr_ctx, matrix.data(), original_channels);
    }

    if (swr_init(swr_ctx) < 0) {
        swr_free(&swr_ctx);
        avcodec_free_context(&codec_ctx);
        std::cerr << "Error: could not initialize resampler" << std::endl;
        return false;
    }

    AVPacket* packet = av_packet_alloc();
    AVFrame*  frame  = av_frame_alloc();
    if (!packet || !frame) {
        av_packet_free(&packet);
        av_frame_free(&frame);
        swr_free(&swr_ctx);
        avcodec_free_context(&codec_ctx);
        std::cerr << "Error: could not allocate packet/frame" << std::endl;
        return false;
    }

    std::vector<float> samples;

    // Lambda: resample one decoded frame and append to samples
    auto resample_frame = [&](AVFrame* f) {
        const int out_count = av_rescale_rnd(
            swr_get_delay(swr_ctx, codec_ctx->sample_rate) + f->nb_samples,
            target_sample_rate,
            codec_ctx->sample_rate,
            AV_ROUND_UP);

        // Allocate temporary output buffer
        uint8_t* out_buf = nullptr;
        int out_linesize = 0;
        if (av_samples_alloc(&out_buf, &out_linesize, out_channels, out_count, AV_SAMPLE_FMT_FLT, 0) < 0) {
            return;
        }

        int converted = swr_convert(swr_ctx,
                                     &out_buf, out_count,
                                     const_cast<const uint8_t**>(f->data), f->nb_samples);
        if (converted > 0) {
            const float* flt = reinterpret_cast<const float*>(out_buf);
            samples.insert(samples.end(), flt, flt + converted * out_channels);
        }
        av_freep(&out_buf);
    };

    // Read & decode packets
    while (av_read_frame(format_ctx, packet) >= 0) {
        if (packet->stream_index == audio_stream_idx) {
            if (avcodec_send_packet(codec_ctx, packet) >= 0) {
                while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
                    resample_frame(frame);
                }
            }
        }
        av_packet_unref(packet);
    }

    // Flush decoder
    avcodec_send_packet(codec_ctx, nullptr);
    while (avcodec_receive_frame(codec_ctx, frame) >= 0) {
        resample_frame(frame);
    }

    // Flush resampler
    {
        int tail = av_rescale_rnd(swr_get_delay(swr_ctx, codec_ctx->sample_rate),
                                  target_sample_rate, codec_ctx->sample_rate, AV_ROUND_UP);
        if (tail > 0) {
            uint8_t* out_buf = nullptr;
            int out_linesize = 0;
            if (av_samples_alloc(&out_buf, &out_linesize, out_channels, tail, AV_SAMPLE_FMT_FLT, 0) >= 0) {
                int converted = swr_convert(swr_ctx, &out_buf, tail, nullptr, 0);
                if (converted > 0) {
                    const float* flt = reinterpret_cast<const float*>(out_buf);
                    samples.insert(samples.end(), flt, flt + converted * out_channels);
                }
                av_freep(&out_buf);
            }
        }
    }

    // Cleanup FFmpeg objects
    av_packet_free(&packet);
    av_frame_free(&frame);
    swr_free(&swr_ctx);
    avcodec_free_context(&codec_ctx);

    // Fill output struct
    out_audio.samples              = std::move(samples);
    out_audio.num_samples          = out_audio.samples.size();
    out_audio.sample_rate          = target_sample_rate;
    out_audio.original_sample_rate = original_sample_rate;
    out_audio.channels             = out_channels;
    out_audio.original_channels    = original_channels;
    out_audio.num_frames           = (out_channels > 0)
                                     ? out_audio.num_samples / out_channels
                                     : 0;
    out_audio.duration_seconds     = (target_sample_rate > 0)
                                     ? static_cast<double>(out_audio.num_frames) / target_sample_rate
                                     : 0.0;

    return out_audio.num_samples > 0;
}

// ---------------------------------------------------------------------------
bool AudioReader::clip_audio_length(audio_data_t& audio, double max_duration_second)
{
    if (audio.sample_rate <= 0 || audio.channels <= 0 || max_duration_second <= 0.0) {
        return false;
    }

    if (audio.duration_seconds <= max_duration_second) {
        return true; // nothing to clip
    }

    size_t max_frames = static_cast<size_t>(max_duration_second * audio.sample_rate);
    size_t max_samples = max_frames * audio.channels;

    if (max_samples < audio.samples.size()) {
        audio.samples.resize(max_samples);
        audio.num_samples = max_samples;
        audio.num_frames = max_frames;
        audio.duration_seconds = static_cast<double>(max_frames) / audio.sample_rate;
    }

    return true;
}

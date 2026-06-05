/// \file image_reader.cpp
/// \brief image_reader class
/// \author FastFlowLM Team
/// \date 2025-08-16
/// \version 0.9.24
/// \note This is a source file for the image_reader functions

#include "image/image_reader.hpp"
#include "typedef.hpp"
#include "base64.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <mutex>
#include "utils/debug_utils.hpp"
// FFmpeg includes for image processing only
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/pixdesc.h>
}

namespace {
static uint32_t read_be32(const uint8_t* ptr) {
    return (static_cast<uint32_t>(ptr[0]) << 24) |
           (static_cast<uint32_t>(ptr[1]) << 16) |
           (static_cast<uint32_t>(ptr[2]) << 8) |
           static_cast<uint32_t>(ptr[3]);
}

static bool has_valid_png_structure(const uint8_t* data, size_t size) {
    if (!data || size < 8) {
        return false;
    }

    static const uint8_t kPngSig[8] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
    if (std::memcmp(data, kPngSig, sizeof(kPngSig)) != 0) {
        return false;
    }

    size_t offset = 8;
    bool saw_ihdr = false;
    bool saw_iend = false;

    while (offset + 12 <= size) {
        const uint32_t chunk_len = read_be32(data + offset);
        const uint8_t* chunk_type = data + offset + 4;
        const size_t full_chunk = static_cast<size_t>(chunk_len) + 12;

        if (offset + full_chunk > size) {
            return false;
        }

        const bool is_ihdr = chunk_type[0] == 'I' && chunk_type[1] == 'H' && chunk_type[2] == 'D' && chunk_type[3] == 'R';
        const bool is_iend = chunk_type[0] == 'I' && chunk_type[1] == 'E' && chunk_type[2] == 'N' && chunk_type[3] == 'D';

        if (!saw_ihdr) {
            if (!is_ihdr || chunk_len != 13) {
                return false;
            }
            saw_ihdr = true;
        }

        if (is_iend) {
            if (chunk_len != 0) {
                return false;
            }
            saw_iend = true;
            return offset + full_chunk == size;
        }

        offset += full_chunk;
    }

    return false;
}

static bool has_valid_jpeg_structure(const uint8_t* data, size_t size) {
    if (!data || size < 4) {
        return false;
    }

    if (!(data[0] == 0xFF && data[1] == 0xD8)) {
        return false;
    }

    bool found_eoi = false;

    for (size_t i = size - 2; i >= 2; --i) {
        if (data[i] == 0xFF && data[i + 1] == 0xD9) {
            found_eoi = true;
            break;
        }
    }

    if (!found_eoi) {
        return false;
    }

    bool saw_sos = false;
    size_t pos = 2;
    while (pos + 1 < size) {
        if (data[pos] != 0xFF) {
            ++pos;
            continue;
        }

        while (pos < size && data[pos] == 0xFF) {
            ++pos;
        }
        if (pos >= size) {
            return false;
        }

        const uint8_t marker = data[pos++];
        if (marker == 0xD9) {
            return saw_sos && (pos == size);
        }

        if (marker == 0xDA) {
            saw_sos = true;
            return true;
        }

        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            continue;
        }

        if (pos + 1 >= size) {
            return false;
        }

        const uint16_t seg_len = static_cast<uint16_t>((data[pos] << 8) | data[pos + 1]);
        if (seg_len < 2) {
            return false;
        }

        pos += static_cast<size_t>(seg_len);
    }

    return false;
}

static bool has_basic_image_integrity(const uint8_t* data, size_t size, int codec_id) {
    if (!data || size == 0) {
        return false;
    }

    if (codec_id == AV_CODEC_ID_PNG) {
        return has_valid_png_structure(data, size);
    }
    if (codec_id == AV_CODEC_ID_MJPEG) {
        return has_valid_jpeg_structure(data, size);
    }
    return false;
}

static void resolve_source_format_and_range(AVPixelFormat input_format,
                                            AVPixelFormat& resolved_format,
                                            int& src_full_range,
                                            AVColorRange frame_color_range,
                                            AVCodecID codec_id) {
    resolved_format = input_format;
    src_full_range = 0;

    if (frame_color_range == AVCOL_RANGE_JPEG || codec_id == AV_CODEC_ID_MJPEG) {
        src_full_range = 1;
    }

    switch (input_format) {
        case AV_PIX_FMT_YUVJ420P:
            resolved_format = AV_PIX_FMT_YUV420P;
            src_full_range = 1;
            break;
        case AV_PIX_FMT_YUVJ422P:
            resolved_format = AV_PIX_FMT_YUV422P;
            src_full_range = 1;
            break;
        case AV_PIX_FMT_YUVJ444P:
            resolved_format = AV_PIX_FMT_YUV444P;
            src_full_range = 1;
            break;
        case AV_PIX_FMT_YUVJ440P:
            resolved_format = AV_PIX_FMT_YUV440P;
            src_full_range = 1;
            break;
        default:
            break;
    }
}
}

ImageMemoryPool::ImageMemoryPool(size_t max_cached_per_size)
    : max_cached_per_size_(max_cached_per_size) {
}

namespace {
static size_t round_up_to(size_t value, size_t alignment) {
    if (alignment == 0) {
        return value;
    }
    return ((value + alignment - 1) / alignment) * alignment;
}

static size_t bucket_size_for_image(size_t requested_size) {
    if (requested_size <= 64 * 1024) {
        return round_up_to(requested_size, 4 * 1024);
    }
    if (requested_size <= 1024 * 1024) {
        return round_up_to(requested_size, 64 * 1024);
    }
    return round_up_to(requested_size, 1024 * 1024);
}
}

bytes ImageMemoryPool::acquire(size_t size) {
    if (size == 0) {
        return bytes();
    }

    const size_t bucket_size = bucket_size_for_image(size);

    std::lock_guard<std::mutex> lock(mutex_);

    auto it = free_lists_.find(bucket_size);
    if (it != free_lists_.end() && !it->second.empty()) {
        bytes block = std::move(it->second.back());
        it->second.pop_back();
        memset(block.data(), 0, block.size());
        return block;
    }

    for (auto candidate = free_lists_.begin(); candidate != free_lists_.end(); ++candidate) {
        if (candidate->first >= bucket_size && !candidate->second.empty()) {
            bytes block = std::move(candidate->second.back());
            candidate->second.pop_back();
            memset(block.data(), 0, block.size());
            return block;
        }
    }

    return bytes(bucket_size);
}

void ImageMemoryPool::recycle(bytes&& block) {
    const size_t size = block.size();
    if (size == 0 || !block.is_owner()) {
        return;
    }

    const size_t bucket_size = bucket_size_for_image(size);

    std::lock_guard<std::mutex> lock(mutex_);
    auto& bucket = free_lists_[bucket_size];
    if (bucket.size() >= max_cached_per_size_) {
        return;
    }
    bucket.push_back(std::move(block));
}

ImageReader::ImageReader(size_t max_cached_per_size)
    : memory_pool_(max_cached_per_size) {
    initialize_ffmpeg();
}

ImageReader::~ImageReader() {
    reset_decode_resources();
}

bool ImageReader::ensure_decode_resources(int codec_id) {
    if (!packet_) {
        packet_ = av_packet_alloc();
    }
    if (!frame_) {
        frame_ = av_frame_alloc();
    }
    if (!rgb_frame_) {
        rgb_frame_ = av_frame_alloc();
    }
    if (!packet_ || !frame_ || !rgb_frame_) {
        return false;
    }

    if (!codec_ctx_ || cached_codec_id_ != codec_id) {
        if (codec_ctx_) {
            avcodec_free_context(&codec_ctx_);
        }

        const AVCodec* codec = avcodec_find_decoder(static_cast<AVCodecID>(codec_id));
        if (!codec) {
            return false;
        }

        codec_ctx_ = avcodec_alloc_context3(codec);
        if (!codec_ctx_) {
            return false;
        }

        codec_ctx_->thread_count = 1;
        codec_ctx_->thread_type = 0;

        if (avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
            avcodec_free_context(&codec_ctx_);
            return false;
        }

        cached_codec_id_ = codec_id;
    }

    return true;
}

void ImageReader::reset_decode_resources() {
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    sws_src_w_ = 0;
    sws_src_h_ = 0;
    sws_src_fmt_ = -1;
    sws_src_range_ = -1;

    if (rgb_frame_) {
        av_frame_free(&rgb_frame_);
    }
    if (frame_) {
        av_frame_free(&frame_);
    }
    if (packet_) {
        av_packet_free(&packet_);
    }
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
    }
    cached_codec_id_ = -1;
}

void ImageReader::initialize_ffmpeg() {
    static std::once_flag init_once;
    std::call_once(init_once, []() {});
}

bool ImageReader::parse_image_header(const uint8_t* data, size_t size, int& codec_id) {
    codec_id = AV_CODEC_ID_NONE;
    if (!data || size < 8) {
        return false;
    }

    if (data[0] == 0xFF && data[1] == 0xD8) {
        codec_id = AV_CODEC_ID_MJPEG;
        return true;
    }
    if (data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47) {
        codec_id = AV_CODEC_ID_PNG;
        return true;
    }
    return false;
}

bool ImageReader::decode_bytes(const uint8_t* data, size_t size, image_data_t& out_image) {
    int codec_id = AV_CODEC_ID_NONE;
    // header_print_g("DEBUG", "Decoding image of size: " << std::to_string(size) << " bytes");
    if (!parse_image_header(data, size, codec_id)) {
        std::cerr << "Error: Unsupported image format (only JPEG/JPG and PNG supported)" << std::endl;
        return false;
    }

    if (!has_basic_image_integrity(data, size, codec_id)) {
        std::cerr << "Error: Image payload failed integrity check" << std::endl;
        return false;
    }

    reset_decode_resources();

    if (!ensure_decode_resources(codec_id)) {
        std::cerr << "Error: Could not initialize FFmpeg decode objects" << std::endl;
        return false;
    }

    bool ok = false;
    do {
        av_frame_unref(frame_);
        av_frame_unref(rgb_frame_);
        av_packet_unref(packet_);

        packet_->data = const_cast<uint8_t*>(data);
        packet_->size = static_cast<int>(size);

        if (avcodec_send_packet(codec_ctx_, packet_) < 0) {
            std::cerr << "Error: Could not send packet for decoding" << std::endl;
            break;
        }

        if (avcodec_receive_frame(codec_ctx_, frame_) < 0) {
            std::cerr << "Error: Could not decode image frame" << std::endl;
            break;
        }

        AVPixelFormat src_fmt = AV_PIX_FMT_NONE;
        int src_full_range = 0;
        resolve_source_format_and_range(static_cast<AVPixelFormat>(frame_->format),
                                        src_fmt,
                                        src_full_range,
                                        frame_->color_range,
                                        codec_ctx_->codec_id);

        const bool need_rebuild_sws = !sws_ctx_ ||
                                      sws_src_w_ != frame_->width ||
                                      sws_src_h_ != frame_->height ||
                                      sws_src_fmt_ != static_cast<int>(src_fmt) ||
                                      sws_src_range_ != src_full_range;
        if (need_rebuild_sws) {
            if (sws_ctx_) {
                sws_freeContext(sws_ctx_);
                sws_ctx_ = nullptr;
            }

            sws_ctx_ = sws_getContext(frame_->width,
                                      frame_->height,
                                      src_fmt,
                                      frame_->width,
                                      frame_->height,
                                      AV_PIX_FMT_RGB24,
                                      SWS_BILINEAR,
                                      nullptr,
                                      nullptr,
                                      nullptr);
            if (sws_ctx_) {
                sws_src_w_ = frame_->width;
                sws_src_h_ = frame_->height;
                sws_src_fmt_ = static_cast<int>(src_fmt);
                sws_src_range_ = src_full_range;
            }
        }

        if (!sws_ctx_) {
            std::cerr << "Error: Could not create scaling context" << std::endl;
            break;
        }

        const AVPixFmtDescriptor* src_desc = av_pix_fmt_desc_get(src_fmt);
        const bool src_is_rgb = src_desc && ((src_desc->flags & AV_PIX_FMT_FLAG_RGB) != 0);
        if (!src_is_rgb) {
            const int* src_coeffs = sws_getCoefficients(SWS_CS_DEFAULT);
            const int* dst_coeffs = sws_getCoefficients(SWS_CS_DEFAULT);
            const int dst_full_range = 1;
            if (sws_setColorspaceDetails(sws_ctx_,
                                         src_coeffs,
                                         src_full_range,
                                         dst_coeffs,
                                         dst_full_range,
                                         0,
                                         1 << 16,
                                         1 << 16) < 0) {
                std::cerr << "Error: Could not configure colorspace conversion" << std::endl;
                break;
            }
        }

        const int rgb_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, frame_->width, frame_->height, 1);
        if (rgb_size <= 0) {
            std::cerr << "Error: Invalid decoded RGB buffer size" << std::endl;
            break;
        }
        // header_print_g("DEBUG", "Decoded image dimensions: " << frame_->width << "x" << frame_->height);
        bytes buffer = memory_pool_.acquire(static_cast<size_t>(rgb_size));
        if (buffer.size() == 0) {
            std::cerr << "Error: Could not allocate output image buffer" << std::endl;
            break;
        }
        // header_print_g("DEBUG", "Acquired buffer of size: " << std::to_string(buffer.size()) << " bytes from memory pool");
        if (av_image_fill_arrays(rgb_frame_->data,
                                 rgb_frame_->linesize,
                                 buffer.data(),
                                 AV_PIX_FMT_RGB24,
                                 frame_->width,
                                 frame_->height,
                                 1) < 0) {
            std::cerr << "Error: Could not initialize RGB output frame" << std::endl;
            memory_pool_.recycle(std::move(buffer));
            break;
        }
        // header_print_g("DEBUG", "Filling RGB image arrays");
        if (sws_scale(sws_ctx_,
                      frame_->data,
                      frame_->linesize,
                      0,
                      frame_->height,
                      rgb_frame_->data,
                      rgb_frame_->linesize) <= 0) {
            std::cerr << "Error: Could not convert decoded frame to RGB24" << std::endl;
            memory_pool_.recycle(std::move(buffer));
            break;
        }
        // header_print_g("DEBUG", "Successfully converted image to RGB24 format");
        recycle(out_image);
        // header_print_g("DEBUG", "Recycling previous output image data if any");
        out_image.width = frame_->width;
        out_image.height = frame_->height;
        out_image.pixels = std::move(buffer);

        // header_print_g("DEBUG", "Image decoding successful!");
        ok = true;
    } while (false);

    av_packet_unref(packet_);

    return ok;
}

bool ImageReader::load_image(const std::string& filename, image_data_t& out_image) {
    initialize_ffmpeg();

    if (!std::filesystem::exists(filename)) {
        std::cerr << "Error: File does not exist: " << filename << std::endl;
        return false;
    }

    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file: " << filename << std::endl;
        return false;
    }

    const std::streamsize file_size = file.tellg();
    if (file_size <= 0) {
        std::cerr << "Error: Invalid image file size" << std::endl;
        return false;
    }
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> data(static_cast<size_t>(file_size));
    if (!file.read(reinterpret_cast<char*>(data.data()), file_size)) {
        std::cerr << "Error: Could not read image file data" << std::endl;
        return false;
    }

    return decode_bytes(data.data(), data.size(), out_image);
}

bool ImageReader::load_image_base64(const std::string& base64_string, image_data_t& out_image) {
    initialize_ffmpeg();

    std::string payload = base64_string;
    const std::size_t comma_pos = payload.find(',');
    if (comma_pos != std::string::npos) {
        payload = payload.substr(comma_pos + 1);
    }

    const std::string decoded = base64::from_base64(payload);
    if (decoded.size() < 8) {
        std::cerr << "Error: Invalid base64 image payload" << std::endl;
        return false;
    }

    return decode_bytes(reinterpret_cast<const uint8_t*>(decoded.data()), decoded.size(), out_image);
}

bool ImageReader::resize_image(const image_data_t& input, int target_width, int target_height, image_data_t& output) {
    if (input.pixels.size() == 0 || input.width <= 0 || input.height <= 0 || target_width <= 0 || target_height <= 0) {
        return false;
    }

    SwsContext* sws_ctx = sws_getContext(input.width,
                                         input.height,
                                         AV_PIX_FMT_RGB24,
                                         target_width,
                                         target_height,
                                         AV_PIX_FMT_RGB24,
                                         SWS_BICUBIC,
                                         nullptr,
                                         nullptr,
                                         nullptr);
    if (!sws_ctx) {
        std::cerr << "Error: Could not create resize context" << std::endl;
        return false;
    }

    const int out_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, target_width, target_height, 1);
    if (out_size <= 0) {
        sws_freeContext(sws_ctx);
        return false;
    }

    bytes out_buffer = memory_pool_.acquire(static_cast<size_t>(out_size));
    if (out_buffer.size() == 0) {
        sws_freeContext(sws_ctx);
        return false;
    }

    uint8_t* src_data[4] = { input.pixels.data(), nullptr, nullptr, nullptr };
    int src_linesize[4] = { input.width * 3, 0, 0, 0 };
    uint8_t* dst_data[4] = { out_buffer.data(), nullptr, nullptr, nullptr };
    int dst_linesize[4] = { target_width * 3, 0, 0, 0 };

    bool ok = sws_scale(sws_ctx,
                        src_data,
                        src_linesize,
                        0,
                        input.height,
                        dst_data,
                        dst_linesize) > 0;
    sws_freeContext(sws_ctx);

    if (!ok) {
        memory_pool_.recycle(std::move(out_buffer));
        return false;
    }

    recycle(output);
    output.width = target_width;
    output.height = target_height;
    output.pixels = std::move(out_buffer);
    return true;
}

bool ImageReader::reorder_hwc_to_chw(const image_data_t& input, image_data_t& output) {
    if (input.pixels.size() == 0 || input.width <= 0 || input.height <= 0) {
        return false;
    }

    const size_t pixel_count = static_cast<size_t>(input.width) * input.height;
    const size_t total_size = pixel_count * 3;
    if (input.pixels.size() < total_size) {
        return false;
    }

    bytes out_buffer = memory_pool_.acquire(total_size);
    if (out_buffer.size() == 0) {
        return false;
    }

    const uint8_t* src = input.pixels.data();
    uint8_t* dst = out_buffer.data();

    for (size_t i = 0; i < pixel_count; ++i) {
        const size_t hwc = i * 3;
        dst[i] = src[hwc];
        dst[pixel_count + i] = src[hwc + 1];
        dst[2 * pixel_count + i] = src[hwc + 2];
    }

    recycle(output);
    output.width = input.width;
    output.height = input.height;
    output.pixels = std::move(out_buffer);
    return true;
}

bool ImageReader::save_png(const std::string& filename, const image_data_t& image) {
    if (image.pixels.size() == 0 || image.width <= 0 || image.height <= 0) {
        return false;
    }

    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (!codec) {
        std::cerr << "Error: PNG encoder not available" << std::endl;
        return false;
    }

    AVCodecContext* codec_ctx = avcodec_alloc_context3(codec);
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();

    if (!codec_ctx || !frame || !pkt) {
        if (pkt) av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        if (codec_ctx) avcodec_free_context(&codec_ctx);
        return false;
    }

    bool ok = false;
    do {
        codec_ctx->codec_type = AVMEDIA_TYPE_VIDEO;
        codec_ctx->codec_id = AV_CODEC_ID_PNG;
        codec_ctx->width = image.width;
        codec_ctx->height = image.height;
        codec_ctx->pix_fmt = AV_PIX_FMT_RGB24;
        codec_ctx->time_base = AVRational{1, 25};

        if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
            std::cerr << "Error: Could not open PNG encoder" << std::endl;
            break;
        }

        frame->format = codec_ctx->pix_fmt;
        frame->width = codec_ctx->width;
        frame->height = codec_ctx->height;

        if (av_frame_get_buffer(frame, 1) < 0) {
            std::cerr << "Error: Could not allocate frame buffer" << std::endl;
            break;
        }

        const uint8_t* src = image.pixels.data();
        for (int y = 0; y < image.height; ++y) {
            std::memcpy(frame->data[0] + y * frame->linesize[0],
                        src + static_cast<size_t>(y) * image.width * 3,
                        static_cast<size_t>(image.width) * 3);
        }

        if (avcodec_send_frame(codec_ctx, frame) < 0) {
            std::cerr << "Error: Could not send frame to PNG encoder" << std::endl;
            break;
        }

        if (avcodec_receive_packet(codec_ctx, pkt) < 0) {
            std::cerr << "Error: Could not receive PNG packet" << std::endl;
            break;
        }

        std::ofstream out(filename, std::ios::binary);
        if (!out.is_open()) {
            std::cerr << "Error: Could not open output file: " << filename << std::endl;
            break;
        }

        out.write(reinterpret_cast<const char*>(pkt->data), pkt->size);
        ok = out.good();
    } while (false);

    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&codec_ctx);
    return ok;
}

void ImageReader::recycle(image_data_t& image) {
    if (image.pixels.size() > 0) {
        memory_pool_.recycle(std::move(image.pixels));
    }
    image.width = 0;
    image.height = 0;
}



bool save_image(const std::string& filename, const bytes& image) {
    try {
        // Calculate dimensions from image size (RGB24 = 3 bytes per pixel)
        if (image.size() % 3 != 0) {
            std::cerr << "Error: Invalid image data size (not divisible by 3)" << std::endl;
            return false;
        }

        int totalPixels = static_cast<int>(image.size() / 3);
        int width = static_cast<int>(sqrt(totalPixels));
        int height = totalPixels / width;

        if (width * height != totalPixels) {
            std::cerr << "Error: Image data does not represent a square image" << std::endl;
            return false;
        }

        // Write PPM format (Portable Pixmap)
        std::ofstream file(filename, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "Error: Could not open file for writing: " << filename << std::endl;
            return false;
        }

        // Write PPM header: "P6\nwidth height\n255\n"
        file << "P6\n" << width << " " << height << "\n255\n";

        // Write RGB data
        file.write(reinterpret_cast<const char*>(image.data()), image.size());
        file.close();

        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Error saving image: " << e.what() << std::endl;
        return false;
    }
}

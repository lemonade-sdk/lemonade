#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace lemon {
namespace audio {

// Supported audio file formats for input
namespace AudioFormat {
    constexpr const char* MP3 = "mp3";
    constexpr const char* MP4 = "mp4";
    constexpr const char* MPEG = "mpeg";
    constexpr const char* MPGA = "mpga";
    constexpr const char* M4A = "m4a";
    constexpr const char* WAV = "wav";
    constexpr const char* WEBM = "webm";
    constexpr const char* OPUS = "opus";
    constexpr const char* AAC = "aac";
    constexpr const char* FLAC = "flac";
}

// Response formats for transcription/translation
namespace ResponseFormat {
    constexpr const char* JSON = "json";
    constexpr const char* TEXT = "text";
    constexpr const char* SRT = "srt";
    constexpr const char* VTT = "vtt";
    constexpr const char* VERBOSE_JSON = "verbose_json";
}

// True for the formats whose successful backend body is a raw text document
// rather than JSON.
inline bool is_plain_text_format(const std::string& response_format) {
    return response_format == ResponseFormat::TEXT ||
           response_format == ResponseFormat::SRT ||
           response_format == ResponseFormat::VTT;
}

inline bool is_supported_response_format(const std::string& response_format) {
    return response_format == ResponseFormat::JSON ||
           response_format == ResponseFormat::VERBOSE_JSON ||
           is_plain_text_format(response_format);
}

// Interpret a successful transcription body from a backend that honours
// response_format. The requested format decides how the body is read, never
// whether it happens to parse: a plain-text body is wrapped verbatim and is
// never handed to the parser, so a transcript that is itself valid JSON
// ("true", "null", "123") is still returned as text. JSON formats parse as
// before and throw on malformed input.
inline nlohmann::json interpret_transcription_body(const std::string& body,
                                                   const std::string& response_format) {
    if (is_plain_text_format(response_format)) {
        return nlohmann::json{{"text", body}};
    }
    return nlohmann::json::parse(body);
}

// Audio-specific error types
namespace ErrorType {
    constexpr const char* AUDIO_FORMAT_UNSUPPORTED = "audio_format_unsupported";
    constexpr const char* AUDIO_PROCESSING_ERROR = "audio_processing_error";
    constexpr const char* AUDIO_FILE_TOO_LARGE = "audio_file_too_large";
    constexpr const char* AUDIO_FILE_INVALID = "audio_file_invalid";
    constexpr const char* AUDIO_LANGUAGE_UNSUPPORTED = "audio_language_unsupported";
}

// Audio file size limits (25MB default, matches OpenAI)
namespace Limits {
    constexpr size_t MAX_FILE_SIZE_BYTES = 25 * 1024 * 1024;  // 25MB
    constexpr double MAX_AUDIO_DURATION_SECONDS = 600.0;       // 10 minutes
}

} // namespace audio
} // namespace lemon

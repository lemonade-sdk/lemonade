#pragma once

#include <string>

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

// Standardized optional request parameters for the audio transcription API.
//
// These supplement the OpenAI-compatible fields (file, model, language, prompt,
// temperature, response_format) with a small, documented set of carrier-audio
// knobs that every transcription backend understands the same way. They are all
// OPTIONAL — when absent the backend keeps its existing behavior, so OpenAI
// compatibility is preserved.
//
// Defined once here and honored by every ITranscriptionServer backend
// (whisper.cpp, sherpa-onnx, ...) via build_transcription_request(), so the API
// surface is uniform regardless of which backend serves the request. Do NOT
// introduce per-backend ad-hoc parameter names; extend this set instead.
namespace RequestParam {
    // Source sample rate of the supplied audio, in Hz (e.g. 8000 for telephony
    // / carrier audio, 16000 for wideband). Backends that need a specific
    // internal rate use this to drive resampling.
    constexpr const char* SAMPLE_RATE = "sample_rate";

    // Source audio bitrate in bits per second (e.g. 64000). Informational /
    // passthrough hint describing compressed carrier audio.
    constexpr const char* AUDIO_BITRATE = "audio_bitrate";

    // Number of channels in the source audio (1 = mono, 2 = stereo). Transducer
    // backends require mono and will downmix when more than one channel is sent.
    constexpr const char* CHANNELS = "channels";

    // BCP-47 / ISO-639 language hint (also part of the OpenAI surface).
    constexpr const char* LANGUAGE = "language";
}

// Default sample rate assumed when a request omits RequestParam::SAMPLE_RATE.
// 16 kHz mono PCM is the canonical input for streaming transducer models.
namespace AudioDefaults {
    constexpr int SAMPLE_RATE_HZ = 16000;
    constexpr int CHANNELS = 1;
}

} // namespace audio
} // namespace lemon

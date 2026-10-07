#include "lemon/server/audio_formats.h"

#include <algorithm>
#include <vector>

#include "lemon/router.h"

namespace lemon {

using json = nlohmann::json;

namespace {

const json& mime_types() {
    static const json kMimeTypes = {
        {"mp3",  "audio/mpeg"},
        {"opus", "audio/opus"},
        {"aac",  "audio/aac"},
        {"flac", "audio/flac"},
        {"wav",  "audio/wav"},
        {"pcm",  "audio/l16;rate=24000;endianness=little-endian"}
    };
    return kMimeTypes;
}

bool contains(const std::vector<std::string>& formats, const std::string& format) {
    return std::find(formats.begin(), formats.end(), format) != formats.end();
}

std::string unsupported_message(const std::string& format,
                                const std::vector<std::string>& supported) {
    std::string supported_list;
    for (const auto& f : supported) {
        supported_list += (supported_list.empty() ? "" : ", ") + f;
    }
    return "response_format '" + format + "' is not supported by this model "
           "(supported: " + supported_list + ")";
}

} // namespace

AudioFormat negotiate_speech_format(Router& router, const std::string& model,
                                    const json& request, bool streaming) {
    AudioFormat result;
    auto supported_formats = router.audio_speech_supported_formats(model);
    if (streaming) {
        auto streaming_formats = router.audio_speech_supported_streaming_formats(model);
        if (!streaming_formats.empty()) {
            supported_formats = std::move(streaming_formats);
        }
    }

    if (request.contains("response_format") && request["response_format"].is_string()) {
        result.format = request["response_format"].get<std::string>();
    } else {
        result.format = streaming ? "pcm" : "mp3";
        if (!supported_formats.empty() && !contains(supported_formats, result.format)) {
            result.format = supported_formats.front();
        }
    }
    if (!mime_types().contains(result.format)) {
        result.error = "Unsupported audio format requested";
        return result;
    }
    if (!supported_formats.empty() && !contains(supported_formats, result.format)) {
        result.error = unsupported_message(result.format, supported_formats);
        return result;
    }

    const auto metadata = router.audio_speech_format_metadata(model, result.format);
    result.mime_type = metadata.content_type.empty()
        ? mime_types()[result.format].get<std::string>()
        : metadata.content_type;
    result.headers.insert(metadata.headers.begin(), metadata.headers.end());
    return result;
}

AudioFormat negotiate_audio_generation_format(Router& router, const std::string& model,
                                              const json& request) {
    AudioFormat result;
    result.format = "wav";
    if (request.contains("response_format") && request["response_format"].is_string()) {
        result.format = request["response_format"].get<std::string>();
    }
    const auto supported_formats = router.audio_generation_supported_formats(model);
    if (!supported_formats.empty() && !contains(supported_formats, result.format)) {
        result.error = unsupported_message(result.format, supported_formats);
        return result;
    }

    const auto metadata = router.audio_generation_format_metadata(model, result.format);
    if (!metadata.content_type.empty()) {
        result.mime_type = metadata.content_type;
    } else if (mime_types().contains(result.format)) {
        result.mime_type = mime_types()[result.format].get<std::string>();
    } else {
        result.mime_type = mime_types()["wav"].get<std::string>();
    }
    result.headers.insert(metadata.headers.begin(), metadata.headers.end());
    return result;
}

} // namespace lemon

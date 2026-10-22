#pragma once

#include <map>
#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "lemon/server/route_error.h"

namespace lemon {

class Router;

// The container a speech or audio-generation response uses depends on what the loaded
// backend can encode, so both routes negotiate it here against the loaded model. A format
// the backend cannot produce is rejected.
// TODO: transcode from a natively supported format instead of rejecting.
struct AudioFormat {
    std::string format;                         // e.g. "mp3"
    std::string mime_type;
    std::map<std::string, std::string> headers; // backend-specific, e.g. sample rate
    std::string error;                          // set when the request cannot be served
};

// A backend's streaming encoder is often narrower than its buffered one, so streaming
// changes which formats apply. An explicit response_format always wins; only the default
// (mp3 buffered, pcm streaming) yields to what the backend declares. response_format is null
// when the request sends none.
AudioFormat negotiate_speech_format(Router& router, const std::string& model,
                                    const nlohmann::json& response_format, bool streaming);

// Only formats the backend produces natively are accepted; the default is wav.
AudioFormat negotiate_audio_generation_format(Router& router, const std::string& model,
                                              const nlohmann::json& response_format);

extern const RouteError kUnsupportedAudioFormat;

// Sets the format's headers, or writes kUnsupportedAudioFormat and returns false.
bool apply_audio_format(const AudioFormat& format, httplib::Response& res);

} // namespace lemon

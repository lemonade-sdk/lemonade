#include "lemon/backends/moonshine/moonshine_hooks.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/audio_mime.h"
#include "lemon/utils/http_client.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>

namespace lemon::backends::moonshine {

namespace {
json forward_audio(const OperationHookContext& context, const std::string& audio_data, const std::string& filename, const json& params) {
    if (audio_data.empty()) {
        throw std::runtime_error("Empty audio data");
    }
    LOG(DEBUG, context.category.c_str()) << "Audio data size: " << audio_data.size() << " bytes" << std::endl;
    const std::filesystem::path filepath(filename);
    std::vector<utils::MultipartField> fields;
    utils::MultipartField audio_file;
    audio_file.name = "file";
    audio_file.data = audio_data;
    audio_file.filename = filepath.filename().string();
    audio_file.content_type = utils::audio_mime_for_extension(filepath.extension().string());
    fields.push_back(audio_file);
    utils::MultipartField format_field;
    format_field.name = "response_format";
    format_field.data = params.value("response_format", "json");
    fields.push_back(format_field);
    for (const char* name : {"language", "prompt"}) {
        if (params.contains(name)) {
            utils::MultipartField field;
            field.name = name;
            field.data = params[name].get<std::string>();
            fields.push_back(field);
        }
    }
    const std::string url = "http://127.0.0.1:" + std::to_string(context.port) + "/inference";
    LOG(DEBUG, context.category.c_str()) << "Sending multipart request to " << url << " (direct data)" << std::endl;
    const auto response = utils::HttpClient::post_multipart(url, fields, 0, utils::HttpSecurityPolicy::TrustedLoopback);
    LOG(DEBUG, context.category.c_str()) << "Response status: " << response.status_code << std::endl;
    if (response.status_code != 200) {
        std::string message = response.body;
        std::string type = "audio_processing_error";
        int status_code = response.status_code;
        try {
            const json error = json::parse(response.body);
            if (error.contains("error")) {
                if (error["error"].is_string()) {
                    message = error["error"].get<std::string>();
                } else if (error["error"].is_object() && error["error"].contains("message")) {
                    message = error["error"]["message"].get<std::string>();
                }
            }
        } catch (...) {
        }
        if (status_code == 400 || (status_code == 500 && message.find("Not a valid RIFF file") != std::string::npos)) {
            status_code = 400;
            type = "invalid_request_error";
        }
        return json{{"error", {{"message", "Transcription failed: " + message}, {"type", type}, {"status_code", status_code}}}};
    }
    try {
        return json::parse(response.body);
    } catch (const json::parse_error&) {
        return json{{"text", response.body}};
    }
}
}

json model_arch(const LaunchHookContext& context) {
    int model_arch = context.model.extra<int>("moonshine_arch", -1);
    if (model_arch < 0) {
        std::string variant = context.model.checkpoint();
        std::transform(variant.begin(), variant.end(), variant.begin(), ::tolower);
        if (variant.find("tiny") != std::string::npos) {
            model_arch = 2;
        } else if (variant.find("small") != std::string::npos) {
            model_arch = 4;
        } else {
            model_arch = 5;
        }
    }
    return model_arch;
}

json transcription(const OperationHookContext& context, const json& request) {
    try {
        if (!request.contains("file_data")) {
            throw std::runtime_error("Missing 'file_data' in request");
        }
        const std::string audio_data = request["file_data"].get<std::string>();
        const std::string filename = request.value("filename", "audio.wav");
        return forward_audio(context, audio_data, filename, request);
    } catch (const std::exception& e) {
        return json{{"error", {{"message", std::string("Transcription failed: ") + e.what()}, {"type", "audio_processing_error"}, {"status_code", 500}}}};
    }
}

}

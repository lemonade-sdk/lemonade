#pragma once

#include "lemon/utils/http_client.h"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace lemon {
namespace backends {
namespace whispercpp {
namespace detail {

inline std::vector<utils::MultipartField> build_multipart_fields(
    utils::MultipartField audio_file,
    const nlohmann::json& params,
    bool translate) {
    std::vector<utils::MultipartField> fields;
    fields.push_back(std::move(audio_file));

    utils::MultipartField fmt_field;
    fmt_field.name = "response_format";
    fmt_field.data = params.value("response_format", "json");
    fields.push_back(std::move(fmt_field));

    utils::MultipartField temp_field;
    temp_field.name = "temperature";
    temp_field.data = params.contains("temperature")
        ? std::to_string(params["temperature"].get<double>())
        : "0.0";
    fields.push_back(std::move(temp_field));

    utils::MultipartField lang_field;
    lang_field.name = "language";
    lang_field.data = params.value("language", "auto");
    fields.push_back(std::move(lang_field));

    if (params.contains("prompt")) {
        utils::MultipartField prompt_field;
        prompt_field.name = "prompt";
        prompt_field.data = params["prompt"].get<std::string>();
        fields.push_back(std::move(prompt_field));
    }

    if (translate) {
        utils::MultipartField translate_field;
        translate_field.name = "translate";
        translate_field.data = "true";
        fields.push_back(std::move(translate_field));
    }

    return fields;
}

}  // namespace detail
}  // namespace whispercpp
}  // namespace backends
}  // namespace lemon

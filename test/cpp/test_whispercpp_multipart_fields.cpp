#include "whispercpp_multipart_fields.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using lemon::backends::whispercpp::detail::build_multipart_fields;
using lemon::utils::MultipartField;
using nlohmann::json;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static size_t field_count(const std::vector<MultipartField>& fields,
                          const std::string& name) {
    return static_cast<size_t>(std::count_if(
        fields.begin(), fields.end(), [&name](const MultipartField& field) {
            return field.name == name;
        }));
}

static const MultipartField* find_field(const std::vector<MultipartField>& fields,
                                        const std::string& name) {
    const auto it = std::find_if(
        fields.begin(), fields.end(), [&name](const MultipartField& field) {
            return field.name == name;
        });
    return it == fields.end() ? nullptr : &*it;
}

static bool field_equals(const std::vector<MultipartField>& fields,
                         const std::string& name,
                         const std::string& data) {
    const MultipartField* field = find_field(fields, name);
    return field != nullptr && field_count(fields, name) == 1 && field->data == data;
}

static MultipartField make_audio_field(const std::string& data,
                                       const std::string& filename) {
    MultipartField field;
    field.name = "file";
    field.data = data;
    field.filename = filename;
    field.content_type = "audio/wav";
    return field;
}

static void omitted_language_defaults_to_auto(const MultipartField& audio_field,
                                              const char* path_name) {
    const std::vector<MultipartField> fields =
        build_multipart_fields(audio_field, json::object(), false);

    const std::string prefix = std::string(path_name) + " omitted language ";
    check((prefix + "keeps the audio field").c_str(),
          field_equals(fields, "file", audio_field.data));
    check((prefix + "adds exactly one auto field").c_str(),
          field_equals(fields, "language", "auto"));
    check((prefix + "keeps response format default").c_str(),
          field_equals(fields, "response_format", "json"));
    check((prefix + "keeps temperature default").c_str(),
          field_equals(fields, "temperature", "0.0"));
}

static void explicit_parameters_are_preserved(const MultipartField& audio_field,
                                              const char* path_name) {
    const json params = {
        {"language", "de"},
        {"prompt", "Noch einmal"},
        {"response_format", "verbose_json"},
        {"temperature", 0.25},
    };
    const std::vector<MultipartField> fields =
        build_multipart_fields(audio_field, params, true);

    const std::string prefix = std::string(path_name) + " explicit parameters ";
    check((prefix + "preserve language").c_str(),
          field_equals(fields, "language", "de"));
    check((prefix + "preserve prompt").c_str(),
          field_equals(fields, "prompt", "Noch einmal"));
    check((prefix + "preserve response format").c_str(),
          field_equals(fields, "response_format", "verbose_json"));
    check((prefix + "preserve temperature").c_str(),
          field_equals(fields, "temperature", std::to_string(0.25)));
    check((prefix + "preserve translation flag").c_str(),
          field_equals(fields, "translate", "true"));
}

int main() {
    const MultipartField file_path_audio =
        make_audio_field("file-path-bytes", "sample-path.wav");
    const MultipartField direct_audio =
        make_audio_field("direct-bytes", "sample-direct.wav");

    omitted_language_defaults_to_auto(file_path_audio, "file path");
    omitted_language_defaults_to_auto(direct_audio, "direct data");
    explicit_parameters_are_preserved(file_path_audio, "file path");
    explicit_parameters_are_preserved(direct_audio, "direct data");

    if (g_failures == 0) {
        std::printf("All whisper.cpp multipart field tests passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}

#include "lemon/backends/ryzenai/ryzenai_request.h"
#include "lemon/error_types.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <string>

using namespace lemon;
using namespace lemon::ryzenai;
using json = nlohmann::json;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) {
        std::cout << "ok: " << message << std::endl;
    } else {
        std::cerr << "FAIL: " << message << std::endl;
        ++failures;
    }
}

void test_string_content() {
    json req = {{"messages", {{{"content", "hello"}}}}};
    json res = normalize_chat_request(req);
    expect(res["messages"][0]["content"] == "hello", "String content unchanged");
}

void test_single_text_part() {
    json req = {{"messages", {{{"content", {{{"type", "text"}, {"text", "hello"}}}}}}}};
    json res = normalize_chat_request(req);
    expect(res["messages"][0]["content"] == "hello", "Single text part");
}

void test_multiple_text_parts() {
    json req = {{"messages", {{{"content", {{{"type", "text"}, {"text", "hello "}}, {{"type", "text"}, {"text", "world"}}}}}}}};
    json res = normalize_chat_request(req);
    expect(res["messages"][0]["content"] == "hello world", "Multiple text parts concatenate");
}

void test_empty_array() {
    json req = {{"messages", {{{"content", json::array()}}}}};
    json res = normalize_chat_request(req);
    expect(res["messages"][0]["content"] == "", "Empty array -> empty string");
}

void test_multiple_messages() {
    json req = {{"messages", {
        {{"content", {{{"type", "text"}, {"text", "m1"}}}}},
        {{"content", "m2"}}
    }}};
    json res = normalize_chat_request(req);
    expect(res["messages"][0]["content"] == "m1", "Multiple messages msg 1");
    expect(res["messages"][1]["content"] == "m2", "Multiple messages msg 2");
}

void test_null_content() {
    json req = {{"messages", {{{"content", nullptr}}}}};
    json res = normalize_chat_request(req);
    expect(res["messages"][0]["content"].is_null(), "Null content unchanged");
}

void test_missing_content() {
    json req = {{"messages", {{{"role", "user"}}}}};
    json res = normalize_chat_request(req);
    expect(res["messages"][0].contains("role") && !res["messages"][0].contains("content"), "Missing content unchanged");
}

void test_top_level_fields() {
    json req = {{"model", "abc"}, {"temperature", 0.5}, {"messages", {{{"content", "hello"}}}}};
    json res = normalize_chat_request(req);
    expect(res["model"] == "abc" && res["temperature"] == 0.5, "Top-level fields preserved");
}

void test_image_url_rejected() {
    json req = {{"messages", {{{"content", {{{"type", "image_url"}, {"image_url", {{"url", "abc"}}}}}}}}}};
    try {
        normalize_chat_request(req);
        expect(false, "image_url should reject");
    } catch (const InvalidRequestException& e) {
        std::string msg = e.what();
        expect(msg.find("messages[0].content[0]") != std::string::npos && msg.find("image_url") != std::string::npos, "image_url rejected with message path");
    }
}

void test_input_audio_rejected() {
    json req = {{"messages", {{{"content", {{{"type", "input_audio"}}}}}}}};
    try {
        normalize_chat_request(req);
        expect(false, "input_audio should reject");
    } catch (const InvalidRequestException& e) {
        std::string msg = e.what();
        expect(msg.find("messages[0].content[0]") != std::string::npos && msg.find("input_audio") != std::string::npos, "input_audio rejected with message path");
    }
}

void test_unknown_type_rejected() {
    json req = {{"messages", {{{"content", {{{"type", "video"}}}}}}}};
    try {
        normalize_chat_request(req);
        expect(false, "unknown type should reject");
    } catch (const InvalidRequestException& e) {
        std::string msg = e.what();
        expect(msg.find("messages[0].content[0]") != std::string::npos && msg.find("video") != std::string::npos, "Unknown type rejected");
    }
}

void test_non_object_part() {
    json req = {{"messages", {{{"content", {"string_in_array"}}}}}};
    try {
        normalize_chat_request(req);
        expect(false, "non-object part should reject");
    } catch (const InvalidRequestException& e) {
        std::string msg = e.what();
        expect(msg.find("messages[0].content[0]") != std::string::npos && msg.find("not an object") != std::string::npos, "Non-object part rejected");
    }
}

void test_missing_text_field() {
    json req = {{"messages", {{{"content", {{{"type", "text"}}}}}}}};
    try {
        normalize_chat_request(req);
        expect(false, "missing text should reject");
    } catch (const InvalidRequestException& e) {
        std::string msg = e.what();
        expect(msg.find("messages[0].content[0]") != std::string::npos && msg.find("missing 'text'") != std::string::npos, "Missing text field rejected");
    }
}

void test_non_string_text_field() {
    json req = {{"messages", {{{"content", {{{"type", "text"}, {"text", 123}}}}}}}};
    try {
        normalize_chat_request(req);
        expect(false, "non-string text should reject");
    } catch (const InvalidRequestException& e) {
        std::string msg = e.what();
        expect(msg.find("messages[0].content[0]") != std::string::npos && msg.find("'text' is not a string") != std::string::npos, "Non-string text field rejected");
    }
}

void test_const_input_not_mutated() {
    const json req = {{"messages", {{{"content", {{{"type", "text"}, {"text", "hello"}}}}}}}};
    json copy = req;
    normalize_chat_request(req);
    expect(req == copy, "Const input not mutated");
}

void test_idempotence() {
    json req = {{"messages", {{{"content", {{{"type", "text"}, {"text", "hello"}}}}}}}};
    json res1 = normalize_chat_request(req);
    json res2 = normalize_chat_request(res1);
    expect(res1 == res2, "Idempotence");
}

int main() {
    test_string_content();
    test_single_text_part();
    test_multiple_text_parts();
    test_empty_array();
    test_multiple_messages();
    test_null_content();
    test_missing_content();
    test_top_level_fields();
    test_image_url_rejected();
    test_input_audio_rejected();
    test_unknown_type_rejected();
    test_non_object_part();
    test_missing_text_field();
    test_non_string_text_field();
    test_const_input_not_mutated();
    test_idempotence();

    if (failures > 0) {
        std::cerr << "Tests failed: " << failures << std::endl;
        return 1;
    }
    std::cout << "All tests passed!" << std::endl;
    return 0;
}

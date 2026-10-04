#include "lemon/backends/llamacpp/llamacpp_systemone.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

using lemon::backends::llamacpp::build_systemone_classify_request;
using lemon::backends::llamacpp::systemone_classify_response;
using nlohmann::json;

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) {
        std::cout << "ok: " << message << std::endl;
        return;
    }
    std::cerr << "FAIL: " << message << std::endl;
    ++failures;
}

template <typename Exception, typename Fn>
bool throws(Fn fn) {
    try {
        fn();
    } catch (const Exception&) {
        return true;
    }
    return false;
}

json answer_with(json probabilities) {
    return json{{"answers", {{"label", {{"type", "choice"}, {"probabilities", probabilities}}}}}};
}

}  // namespace

int main() {
    const json labels = json::array({"coding", "pii", "cooking"});

    const json body = build_systemone_classify_request("bake a cheesecake", labels);
    expect(body["state"] == "bake a cheesecake", "the input text is the state");
    expect(body["questions"].size() == 1, "the body asks exactly one question");
    const json& question = body["questions"]["label"];
    expect(question["type"] == "choice", "the question is a choice");
    expect(question["instructions"].is_string() && !question["instructions"].empty(),
           "the question has instructions");
    expect(question["criteria"] ==
               json({{"coding", nullptr}, {"pii", nullptr}, {"cooking", nullptr}}),
           "each label is an option without a description");

    expect(throws<std::invalid_argument>([] {
               build_systemone_classify_request("x", json::array());
           }),
           "an empty label list is refused");
    expect(throws<std::invalid_argument>([] {
               build_systemone_classify_request("x", json::array({"a", 7}));
           }),
           "a non-string label is refused");
    expect(throws<std::invalid_argument>([] {
               build_systemone_classify_request("x", json::array({"a", "  "}));
           }),
           "a blank label is refused");
    expect(throws<std::invalid_argument>([] {
               build_systemone_classify_request("x", json::array({"a", "a"}));
           }),
           "a duplicate label is refused");

    const json response =
        answer_with({{"coding", 0.0002}, {"pii", 0.0096}, {"cooking", 0.9888}});
    const json all = systemone_classify_response(response, labels, 0);
    expect(all["labels"].size() == 3, "every label is returned without top_k");
    expect(all["labels"]["cooking"] == 0.9888, "the probabilities are returned unchanged");

    const json top = systemone_classify_response(response, labels, 1);
    expect(top["labels"].size() == 1 && top["labels"].contains("cooking"),
           "top_k keeps the highest scores");

    expect(throws<std::runtime_error>([&] {
               systemone_classify_response(answer_with({{"coding", 0.5}, {"pii", 0.5}}), labels, 0);
           }),
           "an answer without one of the labels is refused");
    expect(throws<std::runtime_error>([&] {
               systemone_classify_response(
                   answer_with({{"coding", 0.2}, {"pii", 0.2}, {"other", 0.6}}), labels, 0);
           }),
           "an answer with a label that was not asked is refused");
    expect(throws<std::runtime_error>([&] {
               systemone_classify_response(
                   answer_with({{"coding", 2.0}, {"pii", 0.0}, {"cooking", 0.0}}), labels, 0);
           }),
           "a probability outside [0, 1] is refused");
    expect(throws<std::runtime_error>([&] {
               systemone_classify_response(json{{"answers", json::object()}}, labels, 0);
           }),
           "a response without the answer is refused");

    if (failures != 0) {
        std::cerr << failures << " check(s) failed" << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "all checks passed" << std::endl;
    return EXIT_SUCCESS;
}

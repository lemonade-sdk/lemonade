#include "image_multipart.h"

#include <cstdio>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

httplib::FormData file(const std::string& content,
                       const std::string& filename) {
    httplib::FormData value;
    value.content = content;
    value.filename = filename;
    value.content_type = "image/png";
    return value;
}

}  // namespace

int main() {
    httplib::FormFiles files;
    files.emplace("image", file("primary", "primary.png"));
    files.emplace("image1", file("first-reference", "reference-1.png"));
    files.emplace("image2", file("second-reference", "reference-2.png"));
    files.emplace("mask", file("mask", "mask.png"));

    nlohmann::json request = nlohmann::json::object();
    check(lemon::populate_image_request(files, request),
          "finds the primary image");
    check(request.value("image_data", "") == "cHJpbWFyeQ==",
          "encodes the primary image");
    check(request.value("image_filename", "") == "primary.png",
          "preserves the primary filename");
    check(request.contains("references") && request["references"].is_array(),
          "creates a references array");
    check(request["references"] ==
              nlohmann::json::array({"Zmlyc3QtcmVmZXJlbmNl",
                                     "c2Vjb25kLXJlZmVyZW5jZQ=="}),
          "encodes image-prefixed extras in order");

    httplib::FormFiles array_files;
    array_files.emplace("image[]", file("primary", "primary.png"));
    array_files.emplace("image[]", file("reference", "reference.png"));
    request = nlohmann::json::object();
    check(lemon::populate_image_request(array_files, request),
          "finds an image array primary");
    check(request["references"] ==
              nlohmann::json::array({"cmVmZXJlbmNl"}),
          "preserves extra image array files");

    if (failures == 0) {
        std::printf("All image multipart tests passed\n");
    }
    return failures == 0 ? 0 : 1;
}

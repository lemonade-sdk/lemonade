#include "lemon/jobs/job_media_io.h"

#include <cstdio>

using namespace lemon::jobs;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

int main() {
    bool is_base64 = false;
    std::string content_type = "unchanged";
    check("data URI: base64 payload",
          data_uri_payload("data:image/jpeg;base64,YWJj", &content_type, &is_base64) == "YWJj" &&
              is_base64);
    check("data URI: base64 parameter removed", content_type == "image/jpeg");
    check("data URI: no base64 parameter",
          data_uri_payload("data:image/png,YWJj", &content_type, &is_base64) == "YWJj" &&
              content_type == "image/png" && !is_base64);
    check("data URI: first comma separates payload",
          data_uri_payload("data:image/png;base64,YWJj,rest", nullptr) == "YWJj,rest");
    content_type = "unchanged";
    check("base64: plain string unchanged",
          data_uri_payload("YWJj", &content_type) == "YWJj" && content_type == "unchanged");
    check("data URI: missing comma unchanged",
          data_uri_payload("data:image/png;base64", &content_type) == "data:image/png;base64" &&
              content_type == "unchanged");

    for (const std::string field : {"image", "mask", "image_data", "mask_data"}) {
        json percent_encoded = {{field, "data:image/png,%89PNG%0D%0A"}};
        bool rejected = false;
        try {
            normalize_image_inputs(percent_encoded, true);
        } catch (const JobError& e) {
            const std::string name = field.substr(0, field.find('_'));
            rejected = e.status == 400 && std::string(e.what()) ==
                name + " must be base64 or a base64 data URI";
        }
        check((field + ": percent-encoded URI rejected with field name").c_str(), rejected);
    }
    json invalid_marker = {{"image", "data:image/png;base64url,YWJj"}};
    bool rejected_marker = false;
    try {
        normalize_image_inputs(invalid_marker, true);
    } catch (const JobError& e) {
        rejected_marker = e.status == 400;
    }
    check("image: base64 marker must be a complete parameter", rejected_marker);
    for (const std::string field : {"image", "image_data"}) {
        nlohmann::json percent_encoded = {{field, "data:image/png,%89PNG%0D%0A"}};
        bool rejected = false;
        try {
            normalize_image_inputs(percent_encoded, false);
        } catch (const JobError& e) {
            rejected = e.status == 400 && std::string(e.what()) ==
                "image must be base64 or a base64 data URI";
        }
        check(("3D " + field + ": percent-encoded URI rejected").c_str(), rejected);
    }

    json image = {{"image", "YWJj"}, {"prompt", "keep"}};
    normalize_image_inputs(image, true);
    check("image: base64 mapped", image["image_data"] == "YWJj");
    check("image: defaults", image["image_content_type"] == "image/png" &&
                                image["image_filename"] == "image.png");
    check("image: original fields retained", image["image"] == "YWJj" && image["prompt"] == "keep");
    nlohmann::json uri = {{"image", "data:image/jpeg;base64,YWJj"}};
    normalize_image_inputs(uri, true);
    check("image: URI mapped", uri["image_data"] == "YWJj" &&
                                  uri["image_content_type"] == "image/jpeg");
    check("image: URI filename fallback", uri["image_filename"] == "image.png");
    json explicit_metadata = {{"image", "data:image/jpeg;base64,YWJj"},
                              {"image_filename", "chosen.webp"},
                              {"image_content_type", "image/webp"}};
    normalize_image_inputs(explicit_metadata, true);
    check("image: explicit metadata retained", explicit_metadata["image_filename"] == "chosen.webp" &&
              explicit_metadata["image_content_type"] == "image/webp");
    json mask = {{"mask", "data:image/png;base64,YWJj"}};
    normalize_image_inputs(mask, true);
    check("mask: URI mapped", mask["mask_data"] == "YWJj" && mask["mask_filename"] == "mask.png");
    mask = {{"mask", "YWJj"}, {"mask_filename", "chosen.png"}};
    normalize_image_inputs(mask, true);
    check("mask: base64 and explicit filename", mask["mask_data"] == "YWJj" &&
              mask["mask_filename"] == "chosen.png");
    json existing = {{"image", "ignored"}, {"image_data", "data:image/png;base64,YWJj"},
                     {"mask", "ignored"}, {"mask_data", "data:image/png;base64,YWJk"},
                     {"image_filename", "chosen.png"}, {"mask_filename", "mask.webp"}};
    normalize_image_inputs(existing, true);
    check("backend data: existing image wins and URI stripped", existing["image_data"] == "YWJj");
    check("backend data: existing mask wins and URI stripped", existing["mask_data"] == "YWJk");
    check("backend data: filenames retained", existing["image_filename"] == "chosen.png" &&
              existing["mask_filename"] == "mask.webp");
    existing = {{"image", "ignored"}, {"image_data", "YWJj"}};
    normalize_image_inputs(existing, true);
    check("backend data: plain base64 retained", existing["image_data"] == "YWJj");
    json invalid = {{"image", 42}, {"mask", nullptr}, {"image_data", false}, {"mask_data", 5}};
    const json invalid_before = invalid;
    normalize_image_inputs(invalid, true);
    check("image: non-string fields untouched", invalid == invalid_before);
    json absent = json::object();
    normalize_image_inputs(absent, true);
    check("image: absent fields untouched", absent.empty());

    json model_3d = {{"image_data", "data:image/png;base64,YWJj"}};
    normalize_image_inputs(model_3d, false);
    check("3D: image_data mapped", model_3d["image"] == "YWJj");
    check("3D: source retained", model_3d["image_data"] == "data:image/png;base64,YWJj");
    model_3d = {{"image", "chosen"}, {"image_data", "ignored"}};
    normalize_image_inputs(model_3d, false);
    check("3D: existing image retained", model_3d["image"] == "chosen");
    model_3d["image"] = "data:image/png;base64,YWJj";
    normalize_image_inputs(model_3d, false);
    check("3D: existing image URI stripped", model_3d["image"] == "YWJj");
    model_3d = {{"image_data", "YWJj"}};
    normalize_image_inputs(model_3d, false);
    check("3D: plain base64 mapped", model_3d["image"] == "YWJj");
    model_3d = {{"image", nullptr}, {"image_data", "YWJj"}};
    normalize_image_inputs(model_3d, false);
    check("3D: explicit non-string image retained", model_3d["image"].is_null());

    const json files = json::array({{{"data", "data:image/png;base64,YWJj"},
                                    {"filename", "first.png"}, {"content_type", "image/png"}}});
    json multiple = {{"image_files", files}, {"image", "YWJj"}};
    normalize_image_inputs(multiple, true);
    check("image_files: untouched for images", multiple["image_files"] == files);
    normalize_image_inputs(multiple, false);
    check("image_files: untouched for 3D", multiple["image_files"] == files);

    const auto no_headers = media_result_json("YWJj", 3, "audio/wav", "wav", {});
    check("media result: fields", no_headers == json{{"data", "YWJj"}, {"mime_type", "audio/wav"},
              {"size", 3}, {"response_format", "wav"}});
    check("media result: empty headers omitted", !no_headers.contains("headers"));
    const std::map<std::string, std::string> headers = {
        {"X-MOSS-Channels", "2"}, {"X-MOSS-Sample-Rate", "48000"}};
    const auto pcm = media_result_json("YWJj", 3, "audio/pcm", "pcm", headers);
    check("media result: PCM fields", pcm["data"] == "YWJj" && pcm["size"] == 3 &&
              pcm["mime_type"] == "audio/pcm" && pcm["response_format"] == "pcm");
    check("media result: headers object", pcm["headers"].is_object() &&
              pcm["headers"].size() == 2 && pcm["headers"]["X-MOSS-Channels"] == "2" &&
              pcm["headers"]["X-MOSS-Sample-Rate"] == "48000");

    std::printf("\n%s (%d failures)\n", g_failures ? "FAIL" : "ALL PASS", g_failures);
    return g_failures ? 1 : 0;
}

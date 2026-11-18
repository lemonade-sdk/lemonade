#include "lemon/jobs/job_media_io.h"

#include <cstdio>

using namespace lemon::jobs;

static int g_failures = 0;

static void check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++g_failures;
}

static void test_media_sink_capture() {
    CancelFlag cancel{false};
    const std::string audio("RIFF\0\x01\xff\nWAVE", 12);
    {
        MediaSinkCapture capture(cancel);
        check("capture: initially writable", capture.writable());
        check("capture: empty write accepted", capture.write(nullptr, 0));
        check("capture: audio accepted", capture.write(audio.data(), audio.size()));
        check("capture: audio bytes unchanged", capture.buffer() == audio);
        check("capture: audio remains writable", capture.writable() &&
                  !capture.overflowed() && capture.stream_error().is_null());
        const std::string tail = "event: error\ndata: {\"error\":\"inside binary\"}\n\n";
        check("capture: first chunk fixes binary framing", capture.write(tail.data(), tail.size()) &&
                  capture.buffer() == audio + tail && capture.stream_error().is_null());
    }
    {
        MediaSinkCapture capture(cancel);
        const std::string bytes(kMaxJobMediaOutputBytes, 'a');
        check("capture: exactly 8 MiB accepted", capture.write(bytes.data(), bytes.size()) &&
                  capture.buffer() == bytes && !capture.overflowed() && capture.writable());
        check("capture: one byte over rejected", !capture.write("b", 1) && capture.overflowed());
        check("capture: overflow preserves accepted bytes", capture.buffer() == bytes);
        check("capture: overflow stops writability", !capture.writable());
        check("capture: overflow stops subsequent writes", !capture.write("c", 1) &&
                  !capture.write(nullptr, 0) && capture.buffer() == bytes);
    }
    {
        MediaSinkCapture capture(cancel);
        const std::string bytes(kMaxJobMediaOutputBytes + 1, 'a');
        check("capture: oversized first chunk rejected", !capture.write(bytes.data(), bytes.size()) &&
                  capture.overflowed() && capture.buffer().empty() && !capture.writable());
    }
    for (const std::string error_chunk : {
             "{\"error\":{\"message\":\"backend failed\"}}",
             " \r\ndata: {\"error\":{\"message\":\"backend failed\"}}\n\n"}) {
        MediaSinkCapture capture(cancel);
        const bool accepted = capture.write(audio.data(), audio.size());
        check("capture: error after audio stops write",
              accepted && !capture.write(error_chunk.data(), error_chunk.size()));
        check("capture: error after audio recorded", capture.stream_error() ==
                  nlohmann::json{{"error", {{"message", "backend failed"}}}});
        check("capture: error chunk excluded from media", capture.buffer() == audio);
        check("capture: stream error stops writability", !capture.writable());
        check("capture: stream error stops subsequent writes", !capture.write("more", 4) &&
                  !capture.write(nullptr, 0) && capture.buffer() == audio);
    }
    for (const std::string first : {"event: audio\n", "data: {\"audio\":\"YWJj\"}\n\n"}) {
        MediaSinkCapture capture(cancel);
        const std::string media = "data: {\"audio\":\"YWJj\"}\n\n";
        check("capture: empty write does not choose framing", capture.write(nullptr, 0));
        check("capture: ordinary SSE event/data frames accepted",
              capture.write(first.data(), first.size()) && capture.write(media.data(), media.size()) &&
                  capture.buffer() == first + media && capture.stream_error().is_null() &&
                  capture.writable());
        const std::string prefix = "event: error\ndata: {\"err";
        const std::string suffix = "or\":{\"message\":\"split failure\"}}\r\n\n";
        check("capture: partial SSE error line accepted", capture.write(prefix.data(), prefix.size()) &&
                  capture.stream_error().is_null() && capture.writable());
        check("capture: split SSE error line stops write", !capture.write(suffix.data(), suffix.size()));
        check("capture: split SSE error recorded", capture.stream_error() ==
                  nlohmann::json{{"error", {{"message", "split failure"}}}});
        check("capture: scanned SSE bytes retained", capture.buffer() == first + media + prefix + suffix);
        check("capture: scanned SSE error stops writability", !capture.writable());
        check("capture: scanned SSE error stops later writes", !capture.write("more", 4) &&
                  capture.buffer() == first + media + prefix + suffix);
    }
    {
        MediaSinkCapture capture(cancel);
        const std::string media = "event: audio\ndata: {\"audio\":\"YWJj\"}\n\n";
        const std::string error = "event: error\ndata: {\"error\":\"frame failure\"}\n\n";
        check("capture: media SSE frame accepted", capture.write(media.data(), media.size()));
        check("capture: later complete SSE error frame rejected", !capture.write(error.data(), error.size()));
        check("capture: later SSE error frame recorded with media retained",
              capture.stream_error() == nlohmann::json{{"error", "frame failure"}} &&
                  capture.buffer() == media + error && !capture.writable());
    }
    {
        MediaSinkCapture capture(cancel);
        check("capture: audio before cancellation accepted", capture.write(audio.data(), audio.size()));
        cancel.store(true);
        check("capture: cancellation stops writability", !capture.writable());
        check("capture: cancellation stops writes", !capture.write("more", 4) &&
                  !capture.write(nullptr, 0) && capture.buffer() == audio);
        check("capture: cancellation does not set error or overflow",
              capture.stream_error().is_null() && !capture.overflowed());
    }
}

int main() {
    test_media_sink_capture();

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

#include "lemon/backends/trellis/trellis_hooks.h"
#include "lemon/utils/aixlog.hpp"
#include "lemon/utils/http_client.h"
#include "lemon/utils/image_sniff.h"
#include "lemon/utils/json_utils.h"

#include <string>
#include <vector>

namespace lemon::backends::trellis {

void mesh(const OperationHookContext& context, const json& request, httplib::DataSink& sink) {
    if (!request.contains("image") || !request["image"].is_string()) {
        return;
    }
    std::string b64 = request["image"].get<std::string>();
    auto comma = b64.find(',');
    if (b64.rfind("data:", 0) == 0 && comma != std::string::npos) {
        b64 = b64.substr(comma + 1);
    }
    std::string image = utils::JsonUtils::base64_decode(b64);

    const auto sniffed = utils::sniff_image(image);
    if (!sniffed.ok()) {
        const std::string payload = json{{"error", {
            {"message", "unsupported image format: expected PNG, JPEG, BMP, or GIF"},
            {"type", "invalid_request_error"},
            {"status_code", 400}}}}.dump();
        sink.write(payload.data(), payload.size());
        return;
    }

    std::vector<utils::MultipartField> fields;
    fields.push_back({"image", image, "input." + sniffed.extension, sniffed.mime});
    if (request.contains("seed")) {
        fields.push_back({"seed", std::to_string(request["seed"].get<int>()), "", ""});
    }
    if (request.contains("resolution")) {
        std::string res = request["resolution"].is_string()
                              ? request["resolution"].get<std::string>()
                              : std::to_string(request["resolution"].get<int>());
        fields.push_back({"resolution", res, "", ""});
    }
    if (request.contains("bg_removal") && request["bg_removal"].is_string()) {
        fields.push_back({"bg_removal", request["bg_removal"].get<std::string>(), "", ""});
    }
    if (request.contains("uv") && request["uv"].is_string()) {
        fields.push_back({"uv", request["uv"].get<std::string>(), "", ""});
    }

    auto fail = [&sink](const std::string& message) {
        const std::string payload =
            json{{"error", {{"message", message}, {"type", "backend_error"}}}}.dump();
        sink.write(payload.data(), payload.size());
    };
    try {
        auto resp = utils::HttpClient::post_multipart(
            context.base_url + "/generate",
            fields,
            1800,
            utils::HttpSecurityPolicy::TrustedLoopback);
        if (resp.curl_code != 0) {
            LOG(ERROR, context.category.c_str()) << "generation transport error: " << resp.curl_error << std::endl;
            fail("generation transport error: " + resp.curl_error);
            return;
        }
        if (resp.status_code < 200 || resp.status_code >= 300) {
            LOG(ERROR, context.category.c_str()) << "generation failed (HTTP " << resp.status_code
                                                 << "): " << resp.body << std::endl;
            fail("generation failed (HTTP " + std::to_string(resp.status_code) + ")");
            return;
        }
        sink.write(resp.body.data(), resp.body.size());
    } catch (const std::exception& e) {
        LOG(ERROR, context.category.c_str()) << "model_3d_generations failed: " << e.what() << std::endl;
        fail(e.what());
    }
}

}

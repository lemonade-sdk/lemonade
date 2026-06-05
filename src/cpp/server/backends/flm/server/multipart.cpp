/*!
 *  Copyright (c) 2023 by Contributors
 * \file multipart.cpp
 * \brief MultiPart/form-data Parser
 * \author FastFlowLM Team
 * \date 2025-10-16
 *  \version 0.9.24
 */

#include "multipart.hpp"

///@brief multipart/form-data request parser
///@return parts of multipart/form-data
std::map<std::string, MultipartPart> parse_multipart(const http::request<http::string_body>& req) {
    std::map<std::string, MultipartPart> parts;

    // 1. Extract the boundary from the Content-Type header
    std::string content_type_header = std::string(req[http::field::content_type]);
    std::string boundary;
    size_t boundary_pos = content_type_header.find("boundary=");
    if (boundary_pos != std::string::npos) {
        boundary = "--" + content_type_header.substr(boundary_pos + 9);
    }
    else {
        throw std::runtime_error("Invalid multipart/form-data: boundary not found.");
    }

    std::string_view body = req.body();
    size_t start_pos = 0;

    // 2. Use the boundary to split the request body
    while ((start_pos = body.find(boundary, start_pos)) != std::string_view::npos) {
        start_pos += boundary.length();
        if (body.substr(start_pos, 2) == "--") {
            break; // boundary ended
        }
        start_pos += 2; // skip \r\n

        size_t end_pos = body.find(boundary, start_pos);
        if (end_pos == std::string_view::npos) {
            break;
        }

        std::string_view part_data = body.substr(start_pos, end_pos - start_pos - 2); // 减去 \r\n

        // 3. Parse each part
        size_t headers_end_pos = part_data.find("\r\n\r\n");
        if (headers_end_pos == std::string_view::npos) {
            continue;
        }

        std::string_view headers_sv = part_data.substr(0, headers_end_pos);
        MultipartPart part;
        part.content = std::string(part_data.substr(headers_end_pos + 4));

        // Parse the headers (Content-Disposition)
        size_t cd_pos = headers_sv.find("Content-Disposition: form-data;");
        if (cd_pos != std::string_view::npos) {
            size_t name_pos = headers_sv.find("name=\"", cd_pos);
            if (name_pos != std::string_view::npos) {
                name_pos += 6;
                size_t name_end_pos = headers_sv.find("\"", name_pos);
                part.name = std::string(headers_sv.substr(name_pos, name_end_pos - name_pos));
            }

            size_t filename_pos = headers_sv.find("filename=\"", cd_pos);
            if (filename_pos != std::string_view::npos) {
                filename_pos += 10;
                size_t filename_end_pos = headers_sv.find("\"", filename_pos);
                part.filename = std::string(headers_sv.substr(filename_pos, filename_end_pos - filename_pos));
            }
        }

        if (!part.name.empty()) {
            parts[part.name] = std::move(part);
        }
    }

    return parts;
}

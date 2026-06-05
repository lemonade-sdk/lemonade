/*!
 *  Copyright (c) 2023 by Contributors
 * \file multipart.hpp
 * \brief MultiPart/form-data Parser
 * \author FastFlowLM Team
 * \date 2025-10-16
 *  \version 0.9.24
 */

#pragma once

#include <string>
#include <vector>
#include <map>
#include <string_view>
#include "server.hpp"

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;
using json = nlohmann::ordered_json;

// parts in multipart/form-data
struct MultipartPart {
    std::string name;
    std::string filename;
    std::string content_type;
    std::string content;
};

std::map<std::string, MultipartPart> parse_multipart(const http::request<http::string_body>& req);

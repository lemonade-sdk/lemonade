#pragma once

#include <string>

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace lemon {

struct ServerContext;

bool hybrid_coder_enabled(ServerContext& ctx, const std::string& model);
nlohmann::json run_hybrid_coder(ServerContext& ctx, const nlohmann::json& request);
void stream_hybrid_coder(ServerContext& ctx, const nlohmann::json& request,
                         httplib::DataSink& sink);

} // namespace lemon

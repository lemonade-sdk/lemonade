#pragma once

#include "lemon/server_process.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <httplib.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>

namespace lemon::backends::strata {

inline std::chrono::milliseconds generation_stall_timeout(std::size_t prompt_tokens, bool prefilling, bool sent_stop) {
    const auto standard = std::chrono::minutes(5);
    if (!prefilling || sent_stop) return standard;
    return standard + std::chrono::milliseconds(100 * static_cast<int>(std::min<std::size_t>(prompt_tokens, 6000)));
}

struct VisionCommand {
    ServerCommand command;
    int max_tokens;
};

class StrataProcess : public ServerProcess {
public:
    StrataProcess(std::filesystem::path vocabulary, std::string model_name,
                  std::optional<VisionCommand> vision = std::nullopt,
                  std::function<bool()> load_cancelled = {});
    ~StrataProcess() override;
    std::string start(const ServerCommand& command) override;
    void stop() override;
    bool running() const override;
    bool uses_http() const override { return false; }
    nlohmann::json request(const nlohmann::json& body, bool responses,
                          const std::function<bool()>& cancelled);
    void stream(const nlohmann::json& body, bool responses, httplib::DataSink& sink,
                const std::function<bool()>& cancelled,
                const std::function<void(const nlohmann::json&)>& complete);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}

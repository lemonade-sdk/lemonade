#pragma once

#include <chrono>
#include <functional>
#include <memory>

#include "lemon/server_process.h"
#include "lemon/utils/container_manager.h"

namespace lemon {

struct ContainerProcessServices {
    std::function<bool()> cancelled;
    std::function<std::chrono::steady_clock::time_point()> now;
    std::function<void(std::chrono::milliseconds)> sleep;
};

class ContainerProcess : public ServerProcess {
public:
    ContainerProcess(ProcessOutput output, utils::ContainerRunSpec spec,
                     std::shared_ptr<utils::ContainerManager> manager = {},
                     std::unique_ptr<ServerProcess> client = {},
                     ContainerProcessServices services = {});
    ~ContainerProcess() override;

    std::string start(const ServerCommand& command) override;
    void stop() override;
    bool running() const override;

private:
    utils::ContainerRunSpec spec_;
    std::shared_ptr<utils::ContainerManager> manager_;
    std::unique_ptr<ServerProcess> client_;
    ContainerProcessServices services_;
    bool owned_ = false;
    bool started_ = false;
};

}

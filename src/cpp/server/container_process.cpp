#include "lemon/container_process.h"

#include <algorithm>
#include <stdexcept>
#include <thread>
#include <utility>

#include "lemon/utils/aixlog.hpp"

namespace lemon {

ContainerProcess::ContainerProcess(ProcessOutput output, utils::ContainerRunSpec spec,
                                   std::shared_ptr<utils::ContainerManager> manager,
                                   std::unique_ptr<ServerProcess> client,
                                   ContainerProcessServices services)
    : ServerProcess(output), spec_(std::move(spec)), manager_(std::move(manager)),
      client_(std::move(client)), services_(std::move(services)) {
    if (!spec_.command.empty() || !spec_.env.empty()) {
        throw std::invalid_argument("Container process command and environment come from start()");
    }
    if (!manager_)
        manager_ = std::make_shared<utils::ContainerManager>();
    if (!client_)
        client_ = std::make_unique<NativeProcess>(output);
    if (!services_.cancelled)
        services_.cancelled = [] { return false; };
    if (!services_.now)
        services_.now = [] { return std::chrono::steady_clock::now(); };
    if (!services_.sleep)
        services_.sleep = [](auto duration) { std::this_thread::sleep_for(duration); };
}

ContainerProcess::~ContainerProcess() { stop(); }

std::string ContainerProcess::start(const ServerCommand& command) {
#ifndef __linux__
    throw std::runtime_error("Container backends run on Linux");
#endif
    if (started_)
        throw std::logic_error("Container process has already started");
    if (services_.cancelled())
        throw std::runtime_error("Container start cancelled");
    auto spec = spec_;
    if (spec.port != 0 && spec.port != command.port)
        throw std::invalid_argument("Container port mismatch");
    spec.port = command.port;
    if (!command.program.empty())
        spec.command.push_back(command.program);
    spec.command.insert(spec.command.end(), command.args.begin(), command.args.end());
    spec.env = command.env;
    command_line_ = manager_->run_command(spec);
    started_ = true;
    try {
        manager_->remove_container(spec.name);
        owned_ = true;
        manager_->create_network(spec.network);
        if (services_.cancelled())
            throw std::runtime_error("Container start cancelled");
        ServerCommand invocation;
        invocation.program = command_line_.front();
        invocation.args.assign(command_line_.begin() + 1, command_line_.end());
        client_->start(invocation);
        handle_ = client_->handle();
        if (!client_->running())
            throw std::runtime_error("Container client exited during start");
        if (services_.cancelled())
            throw std::runtime_error("Container start cancelled");
        if (manager_->rootless_podman())
            return "127.0.0.1";
        const auto deadline = services_.now() + std::chrono::seconds(30);
        while (client_->running() && !services_.cancelled() && services_.now() < deadline) {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::seconds>(deadline - services_.now())
                    .count();
            if (remaining < 1)
                break;
            try {
                const auto host = manager_->address(spec.name, spec.network,
                                                    std::min(5, static_cast<int>(remaining)));
                if (!client_->running() || services_.cancelled() || services_.now() >= deadline) {
                    throw std::runtime_error(
                        "Container start interrupted during address discovery");
                }
                return host;
            } catch (const utils::ContainerAddressPending&) {
                services_.sleep(std::chrono::milliseconds(250));
            }
        }
        throw std::runtime_error("Container address unavailable, client exited or start cancelled");
    } catch (...) {
        stop();
        throw;
    }
}

void ContainerProcess::stop() {
    handle_ = {nullptr, 0};
    if (owned_) {
        owned_ = false;
        try {
            manager_->stop(spec_.name);
        } catch (const std::exception& error) {
            LOG(WARNING, "ContainerProcess")
                << "Container cleanup failed: " << error.what() << std::endl;
        }
    }
    if (client_) {
        try {
            client_->stop();
        } catch (const std::exception& error) {
            LOG(WARNING, "ContainerProcess")
                << "Container client cleanup failed: " << error.what() << std::endl;
        }
    }
}

bool ContainerProcess::running() const { return owned_ && client_->running(); }

}

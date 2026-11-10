#pragma once

#include <string>
#include <vector>

namespace lemon {

struct ContainerPinSource {
    std::string source;
    std::string recipe;
    std::string backend;
};

struct ContainerPolicy {
    std::string repository;
    ContainerPinSource pin;
    std::vector<std::string> devices;
    std::vector<std::string> cap_add;
    std::string policy;
    std::string network;
    std::string teardown;
    int stop_grace_seconds = 10;
};

}

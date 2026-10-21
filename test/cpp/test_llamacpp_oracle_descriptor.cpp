#include "llamacpp_oracle_descriptor.h"

#include <filesystem>
#include <fstream>
#include <iostream>

#if LEMON_ORACLE_DESCRIPTOR_OS == 2
#define _WIN32
#endif
#define llamacpp oracle_descriptor_view
#include "lemon/backends/llamacpp/llamacpp.h"
#undef llamacpp
#if LEMON_ORACLE_DESCRIPTOR_OS == 2
#undef _WIN32
#endif

int main(int argc, char** argv) {
    const auto actual = dump_llamacpp_descriptor(lemon::backends::oracle_descriptor_view::descriptor);
    if (argc == 3 && std::string(argv[1]) == "--capture") {
        std::ofstream(argv[2]) << actual.dump(2) << '\n';
        return 0;
    }
    std::ifstream file(std::filesystem::path(LEMON_ORACLE_FIXTURES) / (std::string("descriptor-") + LEMON_ORACLE_VIEW + ".json"));
    const auto expected = nlohmann::json::parse(file);
    const auto difference = nlohmann::json::diff(expected, actual);
    if (!difference.empty()) {
        std::cerr << difference.dump(2) << '\n';
        return 1;
    }
    std::cout << "Complete ordered descriptor matches " << LEMON_ORACLE_VIEW << '\n';
    return 0;
}

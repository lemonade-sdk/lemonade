#include "lemon_tray/tray_app.h"
#include "lemon/cli_parser.h"
#include <iostream>
#include <exception>
#include <lemon/utils/aixlog.hpp>

#ifdef _WIN32
#include <windows.h>
#endif

// Console entry point
// This is the CLI client - perfect for terminal use
int main(int argc, char* argv[]) {
    // Note: Single-instance check moved to serve command specifically
    // This allows status, list, pull, delete, stop to run while server is active

    try {
        lemon::CLIParser parser;
        parser.parse(argc, argv);

        if (!parser.should_continue()) {
            return parser.get_exit_code();
        }

        // Initialize logging
        auto config = parser.get_config();
        auto sink = std::make_shared<AixLog::SinkCout>(AixLog::Filter(AixLog::to_severity(config.log_level)), "%Y-%m-%d %H:%M:%S.#ms [#severity] (#tag_func) #message");
        AixLog::Log::init({sink});

        lemon_tray::TrayApp app(config, parser.get_tray_config());
        return app.run();
    } catch (const std::exception& e) {
        LOG(ERROR) << "Fatal error: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        LOG(ERROR) << "Unknown fatal error" << std::endl;
        return 1;
    }
}

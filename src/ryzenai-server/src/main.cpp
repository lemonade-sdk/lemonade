#include "ryzenai/server.h"
#include "ryzenai/command_line.h"
#include <iostream>
#include <csignal>
#include <memory>

// Global server pointer for signal handling
std::unique_ptr<ryzenai::RyzenAIServer> g_server;

void signalHandler(int signum) {
    std::cout << "\n\n[Main] Interrupt signal (" << signum << ") received." << std::endl;
    
    if (g_server) {
        g_server->stop();
    }
    
    std::exit(signum);
}

int main(int argc, char* argv[]) {
    // Ensure console output works
    std::cout << "Ryzen AI LLM Server starting..." << std::endl;
    std::cout.flush();
    
    try {
        // Register signal handler for graceful shutdown
        std::signal(SIGINT, signalHandler);
        std::signal(SIGTERM, signalHandler);
        
        // Parse command line arguments
        ryzenai::CommandLineArgs args;
        try {
            args = ryzenai::CommandLineParser::parse(argc, argv);
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << "\n\n" << std::flush;
            ryzenai::CommandLineParser::printUsage(argv[0]);
            return 1;
        }
        
        // Validate required arguments
        if (args.model_path.empty()) {
            std::cerr << "Error: Model path is required (-m flag)\n\n";
            ryzenai::CommandLineParser::printUsage(argv[0]);
            return 1;
        }
        
        // Create and run the server
        g_server = std::make_unique<ryzenai::RyzenAIServer>(args);
        g_server->run();
        
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "\n===============================================================\n";
        std::cerr << "FATAL ERROR: " << e.what() << std::endl;
        std::cerr << "===============================================================\n\n";
        return 1;
    }
}


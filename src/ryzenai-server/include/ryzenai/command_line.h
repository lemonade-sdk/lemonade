#pragma once

#include "types.h"
#include <string>

namespace ryzenai {

class CommandLineParser {
public:
    // Parse command line arguments
    static CommandLineArgs parse(int argc, char* argv[]);
    
    // Print usage information
    static void printUsage(const char* program_name);
};

} // namespace ryzenai


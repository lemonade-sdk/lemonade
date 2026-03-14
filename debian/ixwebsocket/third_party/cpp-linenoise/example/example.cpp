#include <iostream>
#include "../linenoise.hpp"

using namespace std;

int main(int argc, const char** argv)
{
    const auto path = "history.txt";

    // Enable the multi-line mode
    linenoise::SetMultiLine(true);

    // Set max length of the history
    linenoise::SetHistoryMaxLen(4);

    // Setup completion words every time when a user types
    linenoise::SetCompletionCallback([](const char* editBuffer, std::vector<std::string>& completions) {
        if (editBuffer[0] == 'h') {
#ifdef _WIN32
            completions.push_back("hello こんにちは");
            completions.push_back("hello こんにちは there");
#else
            completions.push_back("hello");
            completions.push_back("hello there");
#endif
        }
    });

    // Load history
    linenoise::LoadHistory(path);

    while (true) {
        std::string line;
#ifdef _WIN32
        auto quit = linenoise::Readline("hello> ", line);
#else
        auto quit = linenoise::Readline("\033[32mこんにちは\x1b[0m> ", line);
#endif

        if (quit) {
            break;
        }

        cout <<  "echo: '" << line << "'" << endl;

        // Add line to history
        linenoise::AddHistory(line.c_str());

        // Save history
        linenoise::SaveHistory(path);
    }

    return 0;
}

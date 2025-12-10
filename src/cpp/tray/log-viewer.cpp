#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <string>
#include <vector>

// Get parent process ID
DWORD GetParentProcessId() {
    DWORD currentPID = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(pe32);
    DWORD parentPID = 0;

    if (Process32FirstW(snapshot, &pe32)) {
        do {
            if (pe32.th32ProcessID == currentPID) {
                parentPID = pe32.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &pe32));
    }

    CloseHandle(snapshot);
    return parentPID;
}

// Tail a file and print new content
void TailFile(const std::string& filepath, HANDLE parentProcess) {
    // Open file with shared access (FILE_SHARE_DELETE allows installer to delete it)
    HANDLE hFile = CreateFileA(
        filepath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (hFile == INVALID_HANDLE_VALUE) {
        std::cerr << "Error: Could not open log file: " << filepath << std::endl;
        std::cerr << "Press any key to exit..." << std::endl;
        std::cin.get();
        return;
    }

    std::cout << "=== Lemonade Server Log Viewer ===" << std::endl;
    std::cout << "Monitoring: " << filepath << std::endl;
    std::cout << "Press Ctrl+C to exit" << std::endl;
    std::cout << "===================================\n" << std::endl;

    // First, read and display all existing content
    DWORD fileSize = GetFileSize(hFile, nullptr);
    if (fileSize > 0 && fileSize != INVALID_FILE_SIZE) {
        // Limit to last 100KB to avoid overwhelming the console
        DWORD startPos = (fileSize > 102400) ? (fileSize - 102400) : 0;
        SetFilePointer(hFile, startPos, nullptr, FILE_BEGIN);

        std::vector<char> buffer(4096);
        DWORD bytesRead = 0;

        if (startPos > 0) {
            std::cout << "... (showing last 100KB of log file)\n" << std::endl;
        }

        while (ReadFile(hFile, buffer.data(), buffer.size(), &bytesRead, nullptr) && bytesRead > 0) {
            std::cout.write(buffer.data(), bytesRead);
        }
        std::cout.flush();
        std::cout << "\n--- Live tail starting ---\n" << std::endl;
    } else {
        std::cout << "(Log file is empty or new)\n" << std::endl;
        std::cout << "--- Live tail starting ---\n" << std::endl;
    }

    // Get current file position (should be at end after reading historical content)
    DWORD currentPos = SetFilePointer(hFile, 0, nullptr, FILE_CURRENT);

    // Now tail new content
    std::vector<char> buffer(4096);

    while (true) {
        // Check if parent process is still alive
        if (WaitForSingleObject(parentProcess, 0) != WAIT_TIMEOUT) {
            // Parent died, exit gracefully
            break;
        }

        // Check if file has grown
        DWORD currentFileSize = GetFileSize(hFile, nullptr);
        if (currentFileSize != INVALID_FILE_SIZE && currentFileSize > currentPos) {
            // File has new data, seek to where we left off
            SetFilePointer(hFile, currentPos, nullptr, FILE_BEGIN);

            // Read new data
            DWORD bytesToRead = currentFileSize - currentPos;
            DWORD bytesRead = 0;

            while (bytesToRead > 0) {
                DWORD chunkSize = (bytesToRead > buffer.size()) ? buffer.size() : bytesToRead;
                if (ReadFile(hFile, buffer.data(), chunkSize, &bytesRead, nullptr) && bytesRead > 0) {
                    std::cout.write(buffer.data(), bytesRead);
                    std::cout.flush();
                    currentPos += bytesRead;
                    bytesToRead -= bytesRead;
                } else {
                    break;
                }
            }
        }

        // Sleep a bit before checking again
        Sleep(100);
    }

    CloseHandle(hFile);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: log-viewer.exe <log-file-path>" << std::endl;
        return 1;
    }

    std::string logFile = argv[1];

    // Get parent process
    DWORD parentPID = GetParentProcessId();
    if (parentPID == 0) {
        std::cerr << "Error: Could not find parent process" << std::endl;
        return 1;
    }

    HANDLE parentProcess = OpenProcess(SYNCHRONIZE, FALSE, parentPID);
    if (!parentProcess) {
        std::cerr << "Error: Could not open parent process" << std::endl;
        return 1;
    }

    // Tail the file
    TailFile(logFile, parentProcess);

    CloseHandle(parentProcess);
    return 0;
}

// System endpoint handlers for lemon::Server
// These are Server methods extracted to a separate file for organization.
// Handlers: stats, system_info, log_level, shutdown, logs_stream

#include "lemon/server.h"
#include "lemon/system_info.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <cstring>

namespace lemon {

void Server::handle_stats(const httplib::Request& req, httplib::Response& res) {
    // For HEAD requests, just return 200 OK without processing
    if (req.method == "HEAD") {
        res.status = 200;
        return;
    }
    
    try {
        auto stats = router_->get_stats();
        res.set_content(stats.dump(), "application/json");
    } catch (const std::exception& e) {
        std::cerr << "[Server] ERROR in handle_stats: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_system_info(const httplib::Request& req, httplib::Response& res) {
    // For HEAD requests, just return 200 OK without processing
    if (req.method == "HEAD") {
        res.status = 200;
        return;
    }
    
    // Get verbose parameter from query string (default to false)
    bool verbose = false;
    if (req.has_param("verbose")) {
        std::string verbose_param = req.get_param_value("verbose");
        std::transform(verbose_param.begin(), verbose_param.end(), verbose_param.begin(), ::tolower);
        verbose = (verbose_param == "true" || verbose_param == "1");
    }
    
    // Get system info - this function handles all errors internally and never throws
    nlohmann::json system_info = SystemInfoCache::get_system_info_with_cache(verbose);
    res.set_content(system_info.dump(), "application/json");
}

void Server::handle_log_level(const httplib::Request& req, httplib::Response& res) {
    try {
        auto request_json = nlohmann::json::parse(req.body);
        log_level_ = request_json["level"];
        
        nlohmann::json response = {{"status", "success"}, {"level", log_level_}};
        res.set_content(response.dump(), "application/json");
    } catch (const std::exception& e) {
        std::cerr << "[Server] ERROR in handle_log_level: " << e.what() << std::endl;
        res.status = 500;
        nlohmann::json error = {{"error", e.what()}};
        res.set_content(error.dump(), "application/json");
    }
}

void Server::handle_shutdown(const httplib::Request& req, httplib::Response& res) {
    std::cout << "[Server] Shutdown request received" << std::endl;
    
    nlohmann::json response = {{"status", "shutting down"}};
    res.set_content(response.dump(), "application/json");
    
    // Stop the server asynchronously to allow response to be sent
    std::thread([this]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::cout << "[Server] Stopping server..." << std::endl;
        std::cout.flush();
        stop();
        
        // Graceful shutdown with timeout: explicitly unload models and stop backend servers
        if (router_) {
            std::cout << "[Server] Unloading models and stopping backend servers..." << std::endl;
            std::cout.flush();
            
            // Just call unload_model directly - keep it simple
            try {
                router_->unload_model();
                std::cout << "[Server] Cleanup completed successfully" << std::endl;
                std::cout.flush();
            } catch (const std::exception& e) {
                std::cerr << "[Server] Error during unload: " << e.what() << std::endl;
                std::cerr.flush();
            }
        }
        
        // Force process exit - just use standard exit()
        std::cout << "[Server] Calling exit(0)..." << std::endl;
        std::cout.flush();
        std::exit(0);
    }).detach();
}

void Server::handle_logs_stream(const httplib::Request& req, httplib::Response& res) {
    // Check if log file exists
    if (log_file_path_.empty() || !std::filesystem::exists(log_file_path_)) {
        std::cerr << "[Server] Log file not found: " << log_file_path_ << std::endl;
        std::cerr << "[Server] Note: Log streaming only works when server is launched via tray/ServerManager" << std::endl;
        res.status = 404;
        nlohmann::json error = {
            {"error", "Log file not found. Log streaming requires server to be launched via tray application."},
            {"path", log_file_path_},
            {"note", "When running directly, logs appear in console instead."}
        };
        res.set_content(error.dump(), "application/json");
        return;
    }
    
    std::cout << "[Server] Starting log stream for: " << log_file_path_ << std::endl;
    
    // Set SSE headers
    res.set_header("Content-Type", "text/event-stream");
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");
    res.set_header("X-Accel-Buffering", "no");
    
    // Use chunked streaming
    res.set_chunked_content_provider(
        "text/event-stream",
        [this](size_t offset, httplib::DataSink& sink) {
            // Thread-local state for this connection
            static thread_local std::unique_ptr<std::ifstream> log_stream;
            static thread_local std::streampos last_pos = 0;
            
            if (offset == 0) {
                // First call: open file and read from beginning
                log_stream = std::make_unique<std::ifstream>(
                    log_file_path_, 
                    std::ios::in
                );
                
                if (!log_stream->is_open()) {
                    std::cerr << "[Server] Failed to open log file for streaming" << std::endl;
                    return false;
                }
                
                // Start from beginning
                log_stream->seekg(0, std::ios::beg);
                last_pos = 0;
                
                std::cout << "[Server] Log stream connection opened" << std::endl;
            }
            
            // Seek to last known position
            log_stream->seekg(last_pos);
            
            std::string line;
            bool sent_data = false;
            int lines_sent = 0;
            
            // Read and send new lines
            while (std::getline(*log_stream, line)) {
                // Format as SSE: "data: <line>\n\n"
                std::string sse_msg = "data: " + line + "\n\n";
                
                if (!sink.write(sse_msg.c_str(), sse_msg.length())) {
                    std::cout << "[Server] Log stream client disconnected" << std::endl;
                    return false;  // Client disconnected
                }
                
                sent_data = true;
                lines_sent++;
                
                // CRITICAL: Update position after each successful line read
                // Must do this BEFORE hitting EOF, because tellg() returns -1 at EOF!
                last_pos = log_stream->tellg();
            }
            
            // Clear EOF and any other error flags so we can continue reading on next poll
            log_stream->clear();
            
            // Send heartbeat if no data (keeps connection alive)
            if (!sent_data) {
                const char* heartbeat = ": heartbeat\n\n";
                if (!sink.write(heartbeat, strlen(heartbeat))) {
                    std::cout << "[Server] Log stream client disconnected during heartbeat" << std::endl;
                    return false;
                }
            }
            
            // Sleep briefly before next poll
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            
            return true;  // Keep streaming
        }
    );
}

} // namespace lemon


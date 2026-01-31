#include "lemon/backends/kokoro_server.h"
#include "lemon/backends/backend_utils.h"
#include "lemon/audio_types.h"
#include "lemon/utils/http_client.h"
#include "lemon/utils/process_manager.h"
#include "lemon/utils/path_utils.h"
#include "lemon/utils/json_utils.h"
#include "lemon/error_types.h"
#include <httplib.h>
#include <iostream>
#include <fstream>
#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace lemon::utils;

namespace lemon {
namespace backends {

KokoroServer::KokoroServer(const std::string& log_level, ModelManager* model_manager)
    : WrappedServer("kokoro-server", log_level, model_manager) {

}

KokoroServer::~KokoroServer() {
    unload();
}

// WrappedServer interface
void KokoroServer::install(const std::string& backend) {

}

std::string KokoroServer::download_model(const std::string& checkpoint, const std::string& mmproj, bool do_not_upgrade) {
    return "";
}

void KokoroServer::load(const std::string& model_name, const ModelInfo& model_info, const RecipeOptions& options, bool do_not_upgrade) {

}

void KokoroServer::unload() {

}

// ICompletionServer implementation (not supported - return errors)
json KokoroServer::chat_completion(const json& request) {
    return json{
        {"error", {
            {"message", "Kokoro does not support text completion. Use audio speech endpoints instead."},
            {"type", "unsupported_operation"},
            {"code", "model_not_applicable"}
        }}
    };
}

json KokoroServer::completion(const json& request) {
    return json{
        {"error", {
            {"message", "Kokoro does not support text completion. Use audio speech endpoints instead."},
            {"type", "unsupported_operation"},
            {"code", "model_not_applicable"}
        }}
    };
}

json KokoroServer::responses(const json& request) {
    return json{
        {"error", {
            {"message", "Kokoro does not support text completion. Use audio speech endpoints instead."},
            {"type", "unsupported_operation"},
            {"code", "model_not_applicable"}
        }}
    };
}

json KokoroServer::audio_speech(const json& request) {
    return json::object();
}

std::string KokoroServer::get_kokoro_server_path() { return ""; }
std::string KokoroServer::find_executable_in_install_dir(const std::string& install_dir) { return ""; }
std::string KokoroServer::find_external_kokoro_server() { return ""; }

} // namespace backends
} // namespace lemon

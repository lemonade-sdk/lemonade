// The facade's job is orchestration. Every rule below is a ratchet: the ceiling is what
// the file does today, and a count under it fails too, so the number can only travel in
// one direction. Adding to the facade is allowed only by naming what moved in.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef LEMONADE_REPO_ROOT
#error "LEMONADE_REPO_ROOT must name the repository root"
#endif

namespace fs = std::filesystem;

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

namespace {

const fs::path kRoot = LEMONADE_REPO_ROOT;
const std::string kFacade = "src/cpp/server/model_manager.cpp";
const std::string kFacadeHeader = "src/cpp/include/lemon/model_manager.h";

std::string read_repo_file(const std::string& relative) {
    std::ifstream in(kRoot / relative, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// One hit per line, reported with its line number so a failure says where to look.
struct Hit {
    std::string file;
    int line;
};

std::vector<Hit> find_in(const std::string& relative, const std::string& needle) {
    std::vector<Hit> hits;
    std::ifstream in(kRoot / relative, std::ios::binary);
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        if (line.find(needle) != std::string::npos) hits.push_back({relative, number});
    }
    return hits;
}

void ratchet(const std::string& what, const std::string& file, const std::string& needle,
             int ceiling) {
    const std::vector<Hit> hits = find_in(file, needle);
    const int count = static_cast<int>(hits.size());
    std::ostringstream detail;
    if (count > ceiling) {
        for (const auto& hit : hits) detail << "\n    " << hit.file << ":" << hit.line;
        check(what + " (found " + std::to_string(count) + ", ceiling " +
                  std::to_string(ceiling) + detail.str() + ")",
              false);
        return;
    }
    if (count < ceiling) {
        check(what + " — ceiling is stale at " + std::to_string(ceiling) +
                  ", tighten to " + std::to_string(count),
              false);
        return;
    }
    check(what + " (" + std::to_string(count) + ")", true);
}

std::vector<std::string> repo_files(const std::string& subdir) {
    std::vector<std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(kRoot / subdir)) {
        if (!entry.is_regular_file()) continue;
        const std::string ext = entry.path().extension().string();
        if (ext != ".h" && ext != ".cpp") continue;
        files.push_back(fs::relative(entry.path(), kRoot).string());
    }
    std::sort(files.begin(), files.end());
    return files;
}

bool in_models_layer(const std::string& file) {
    return file.rfind("src/cpp/server/models/", 0) == 0 ||
           file.rfind("src/cpp/include/lemon/models/", 0) == 0;
}

}  // namespace

int main() {
    const std::string facade = read_repo_file(kFacade);
    check("the facade is read from the tree", facade.size() > 1000);

    // A path may pass through; opening one does not.
    ratchet("the facade names a path and does no more", kFacade, "fs::", 3);
    ratchet("the facade opens no directory", kFacade, "directory_iterator", 0);
    ratchet("the facade runs no filesystem operation", kFacade, "fs::exists(", 0);
    ratchet("the facade removes nothing", kFacade, "fs::remove", 0);
    ratchet("the facade parses no registry row", kFacade, "contains(\"", 0);
    ratchet("the facade reads no JSON value by hand", kFacade, "get<std::string>()", 0);
    ratchet("the facade parses no document", kFacade, "json::parse", 0);
    ratchet("the facade builds no repository cache path", kFacade, "repo_id_to_cache_dir_name", 0);
    ratchet("the facade does not probe the machine", kFacade, "probe_backend_availability", 0);
    // One seam asks a recipe's ops what models it can see; anything beyond that and the
    // facade is back to branching on backends.
    ratchet("the facade dispatches to backend ops once", kFacade, "ops_for(", 1);
    ratchet("the facade declares no test hook", kFacade, "_for_test", 0);

    std::vector<std::string> allowed_hooks = {
        "discover_extra_models_for_test",
        "set_model_update_available_for_test",
        "set_update_check_override_for_test",
        "set_download_model_override_for_test",
    };
    int declared_hooks = 0;
    for (const auto& hook : allowed_hooks) {
        declared_hooks += static_cast<int>(find_in(kFacadeHeader, hook).size());
    }
    int total_hooks = 0;
    for (const auto& file : repo_files("src/cpp")) {
        total_hooks += static_cast<int>(find_in(file, "_for_test").size());
    }
    check("the public hooks are the four already named (" + std::to_string(total_hooks) + ")",
          total_hooks == declared_hooks);

    std::vector<std::string> escaping;
    for (const auto& file : repo_files("src/cpp")) {
        if (in_models_layer(file) || file == kFacade || file == kFacadeHeader) continue;
        if (!find_in(file, "include <lemon/models/").empty()) escaping.push_back(file);
    }
    const std::vector<std::string> allowed_escaping = {"src/cpp/server/registry_files.cpp"};
    check("only registry_files.cpp reaches past the facade header",
          escaping == allowed_escaping);
    if (escaping != allowed_escaping) {
        for (const auto& file : escaping) std::printf("    %s\n", file.c_str());
    }

    // The measure this refactor was opened for, kept as a ceiling rather than a target.
    const int lines = static_cast<int>(std::count(facade.begin(), facade.end(), '\n'));
    constexpr int kLineCeiling = 1103;
    check("the facade is " + std::to_string(lines) + " lines, ceiling " +
              std::to_string(kLineCeiling),
          lines <= kLineCeiling);
    if (lines < kLineCeiling) {
        check("its line ceiling is stale, tighten to " + std::to_string(lines), false);
    }

    if (g_failures > 0) {
        std::printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("\nAll facade-shape checks passed\n");
    return 0;
}

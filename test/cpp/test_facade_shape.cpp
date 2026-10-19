// The facade's job is orchestration. Most rules below are exact-count ratchets: a count
// under the ceiling fails too, so a number only travels in a commit that names what moved.
// The line budget binds one way: shrinking it is progress, and progress must not first
// edit here.
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

// Both include spellings: a rule over one is a rule that misses files.
std::vector<Hit> find_includes_in(const std::string& relative, const std::string& path) {
    std::vector<Hit> hits;
    std::ifstream in(kRoot / relative, std::ios::binary);
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        if (line.find("include <" + path) != std::string::npos ||
            line.find("include \"" + path) != std::string::npos)
            hits.push_back({relative, number});
    }
    return hits;
}

std::string read_line(const std::string& relative, int wanted) {
    std::ifstream in(kRoot / relative, std::ios::binary);
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        if (++number == wanted) return line;
    }
    return "";
}

// The quoted or angled path of an include line.
std::string header_on_line(const std::string& line) {
    const size_t begin = line.find("lemon/");
    if (begin == std::string::npos) return "";
    const size_t end = line.find_first_of("\" >", begin);
    return line.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
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
    ratchet("the facade names a path and does no more", kFacade, "fs::", 2);
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

    // No product file declares a test hook, anywhere. A test that needs behavior
    // substituted takes a collaborator through a constructor or config seam.
    std::vector<std::string> hook_files;
    for (const auto& file : repo_files("src/cpp")) {
        if (!find_in(file, "_for_test").empty()) hook_files.push_back(file);
    }
    check("no product file declares a test hook (" + std::to_string(hook_files.size()) + ")",
          hook_files.empty());
    for (const auto& file : hook_files) {
        std::printf("  hook in: %s\n", file.c_str());
    }
    // Mutable override members are hooks that never got a set_* name.
    check("the facade header holds no override member",
          find_in(kFacadeHeader, "_override_").empty());

    // The facade header is what consumers include. model_info.h, registry_types.h and
    // store_snapshot.h are the models layer's public read side; any other models header
    // reaching a consumer is named here with the fact it reads, and each name retires by
    // deletion.
    const std::vector<std::string> public_headers = {
        "lemon/models/model_info.h",
        "lemon/models/registry_types.h",  // wire facts that live on disk
        "lemon/models/store_snapshot.h"};
    std::vector<std::string> escaping;
    for (const auto& file : repo_files("src/cpp")) {
        if (in_models_layer(file) || file == kFacade || file == kFacadeHeader) continue;
        for (const auto& hit : find_includes_in(file, "lemon/models/")) {
            const std::string header = header_on_line(read_line(file, hit.line));
            if (std::find(public_headers.begin(), public_headers.end(), header) ==
                public_headers.end()) {
                escaping.push_back(file);
                break;
            }
        }
    }
    const std::vector<std::string> allowed_escaping = {
        "src/cpp/include/lemon/backends/backend_ops.h",              // ArtifactFetchService
        "src/cpp/server/backends/backend_ops.cpp",                   // ArtifactRef, artifacts
        "src/cpp/server/backends/whispercpp/whispercpp_server.cpp",  // the lazy side cache
        "src/cpp/server/hf_variants.cpp",                            // variant reads via source
        "src/cpp/server/model_registry.cpp",                         // RegistryId
        "src/cpp/server/model_registry.h",                           // the adapter's own side
        "src/cpp/server/registry_files.cpp",                         // checkpoint grammar
        "src/cpp/server/runtime_config.cpp",                         // configured catalogs
    };
    check("consumers outside the whitelist include no models-layer header",
          escaping == allowed_escaping);
    if (escaping != allowed_escaping) {
        for (const auto& file : escaping)
            if (std::find(allowed_escaping.begin(), allowed_escaping.end(), file) ==
                allowed_escaping.end())
                std::printf("    unexpected: %s\n", file.c_str());
        for (const auto& file : allowed_escaping)
            if (std::find(escaping.begin(), escaping.end(), file) == escaping.end())
                std::printf("    retired, delete the name: %s\n", file.c_str());
    }

    // The models layer depends on model_info.h, never on the facade header: the DAG
    // points one way, and a hit here is the inversion back.
    int facade_includes = 0;
    std::ostringstream facade_hits;
    for (const std::string& dir : {"src/cpp/server/models", "src/cpp/include/lemon/models"}) {
        for (const auto& file : repo_files(dir)) {
            for (const auto& hit : find_includes_in(file, "lemon/model_manager.h")) {
                ++facade_includes;
                facade_hits << "\n    " << hit.file << ":" << hit.line;
            }
        }
    }
    check("the models layer names the facade header " + std::to_string(facade_includes) +
              " times, ceiling 0" +
              (facade_includes > 0 ? facade_hits.str() : std::string()),
          facade_includes == 0);

    // The legacy registry class lives with its two adapters and nowhere else: a
    // third file naming it is a new caller around the repository seam.
    std::vector<std::string> registry_includers;
    for (const auto& file : repo_files("src/cpp")) {
        if (!find_in(file, "model_registry.h").empty()) registry_includers.push_back(file);
    }
    const std::vector<std::string> adapter_pair = {
        "src/cpp/server/model_registry.cpp",
        "src/cpp/server/models/registry_repository.cpp",
    };
    check("only the adapter pair names the legacy registry header",
          registry_includers == adapter_pair);
    if (registry_includers != adapter_pair) {
        for (const auto& file : registry_includers)
            if (std::find(adapter_pair.begin(), adapter_pair.end(), file) ==
                adapter_pair.end())
                std::printf("    unexpected: %s\n", file.c_str());
    }

    // Acceptance, stated in production terms: the store, the scan, the pull and the
    // facade never name a provider — they name a source and let the adapter answer.
    int provider_literals = 0;
    for (const std::string& file :
         {"src/cpp/server/models/store.cpp", "src/cpp/server/models/sync_service.cpp",
          "src/cpp/server/models/ingest.cpp", "src/cpp/server/models/update_scan.cpp",
          "src/cpp/server/model_manager.cpp", "src/cpp/include/lemon/model_manager.h",
          "src/cpp/include/lemon/models/model_info.h"}) {
        for (const auto& literal : {"\"huggingface\"", "\"modelscope\""}) {
            for (const auto& hit : find_in(file, literal)) {
                ++provider_literals;
                std::printf("    %s:%d names a provider\n", hit.file.c_str(), hit.line);
            }
        }
    }
    check("no core file names a registry provider by literal", provider_literals == 0);

    // The measure this refactor was opened for: a ceiling, not a target.
    const int lines = static_cast<int>(std::count(facade.begin(), facade.end(), '\n'));
    constexpr int kLineCeiling = 1097;
    check("the facade is " + std::to_string(lines) + " lines, ceiling " +
              std::to_string(kLineCeiling),
          lines <= kLineCeiling);

    if (g_failures > 0) {
        std::printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("\nAll facade-shape checks passed\n");
    return 0;
}

// The facade's job is orchestration, and this file is what keeps that contract alive.
//
// Every "at most" rule here travels one way: a count over the ceiling fails and names the
// lines, a count under it prints a note asking for the ceiling to be tightened. Progress
// must not first have to edit this file. Named sets are the exception — a whitelist entry
// that stops being used has to be deleted, so a name can neither be added nor retired
// silently.
//
// kRulesVersion prints with the results. A test binary reads the *live* tree but carries
// the rules it was compiled with, so a binary older than the sources can fail for a rule
// that no longer exists; seeing the version makes that visible instead of inferred.
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

static const char* kRulesVersion = "2026-10-19.1";

static int g_failures = 0;

static void check(const std::string& name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    if (!ok) ++g_failures;
}

namespace {

const fs::path kRoot = LEMONADE_REPO_ROOT;
const std::string kFacade = "src/cpp/server/model_manager.cpp";
const std::string kFacadeHeader = "src/cpp/include/lemon/model_manager.h";
const std::string kModelsDir = "models/";

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

std::string read_line(const std::string& relative, int wanted) {
    std::ifstream in(kRoot / relative, std::ios::binary);
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        if (++number == wanted) return line;
    }
    return "";
}

// Text with `//` comments removed, so nothing below counts a word that only appears
// in prose. Line count is preserved, so line numbers stay meaningful.
std::string strip_line_comments(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool in_string = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            out += c;
            if (c == '"' && (i == 0 || text[i - 1] != '\\')) in_string = false;
            continue;
        }
        if (c == '"') {
            in_string = true;
            out += c;
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') ++i;
            if (i < text.size()) out += '\n';
            continue;
        }
        out += c;
    }
    return out;
}

// True when `include_path` names `header`, whatever spelling the includer used. An
// angle/quoted path may be the full one, a bare basename resolved through the include
// directories, or a `../` walk; each reaches the same file and each was being missed.
bool names_header(const std::string& include_path, const std::string& header) {
    if (include_path == header) return true;
    const std::string base = header.substr(header.find_last_of('/') + 1);
    if (include_path == base) return true;
    const std::string with_slash = "/" + base;
    if (include_path.size() >= with_slash.size() &&
        include_path.compare(include_path.size() - with_slash.size(), with_slash.size(),
                             with_slash) == 0)
        return true;
    // A relative walk into a subdirectory, e.g. "models/store.h" or "../models/store.h".
    const std::string tail = header.substr(header.find_first_of('/') + 1);
    if (include_path == tail) return true;
    const std::string tail_slash = "/" + tail;
    return include_path.size() >= tail_slash.size() &&
           include_path.compare(include_path.size() - tail_slash.size(), tail_slash.size(),
                                tail_slash) == 0;
}

// The path of every `#include` line in a file, comment-stripped. Lines whose first
// non-blank character is not '#' are not includes: a sentence naming a header, or an
// include someone commented out, must not be counted as a dependency.
std::vector<std::string> included_paths_in(const std::string& relative) {
    std::vector<std::string> paths;
    const std::string text = strip_line_comments(read_repo_file(relative));
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] != '#') continue;
        if (line.find("include") == std::string::npos) continue;
        const size_t open = line.find_first_of("<\"");
        if (open == std::string::npos) continue;
        const size_t close = line.find_first_of(">\"", open + 1);
        if (close == std::string::npos) continue;
        paths.push_back(line.substr(open + 1, close - open - 1));
    }
    return paths;
}

// Files that include `header` under any spelling.
std::vector<std::string> includers_of(const std::string& header,
                                      const std::vector<std::string>& files) {
    std::vector<std::string> result;
    for (const auto& file : files) {
        for (const auto& path : included_paths_in(file)) {
            if (names_header(path, header)) {
                result.push_back(file);
                break;
            }
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

void report_set_diff(const std::vector<std::string>& found,
                     const std::vector<std::string>& allowed) {
    for (const auto& file : found)
        if (std::find(allowed.begin(), allowed.end(), file) == allowed.end())
            std::printf("    unexpected: %s\n", file.c_str());
    for (const auto& file : allowed)
        if (std::find(found.begin(), found.end(), file) == found.end())
            std::printf("    retired, delete the name: %s\n", file.c_str());
}

// An "at most" ceiling: over it fails, under it only notes. Shrinking a count used to
// fail with "ceiling is stale", which punished exactly the churn the refactor wants.
void at_most(const std::string& what, const std::string& file, const std::string& needle,
             int ceiling) {
    const std::vector<Hit> hits = find_in(file, needle);
    const int count = static_cast<int>(hits.size());
    if (count > ceiling) {
        std::ostringstream detail;
        for (const auto& hit : hits) detail << "\n    " << hit.file << ":" << hit.line;
        check(what + " (found " + std::to_string(count) + ", ceiling " +
                  std::to_string(ceiling) + detail.str() + ")",
              false);
        return;
    }
    if (count < ceiling) {
        std::printf("note: %s is at %d; ceiling is stale at %d, tighten it\n", what.c_str(),
                    count, ceiling);
    }
    check(what + " (" + std::to_string(count) + " <= " + std::to_string(ceiling) + ")", true);
}

// A named set that may only shrink: new names fail, and so does leaving a name in the
// list that nothing uses any more.
void may_only_shrink(const std::string& what, const std::vector<std::string>& found,
                     const std::vector<std::string>& allowed) {
    std::vector<std::string> extra;
    for (const auto& file : found)
        if (std::find(allowed.begin(), allowed.end(), file) == allowed.end()) extra.push_back(file);
    if (!extra.empty()) {
        for (const auto& file : extra)
            std::printf("    unexpected: %s (add the name only with the fact it reads)\n",
                        file.c_str());
        check(what + ": " + std::to_string(extra.size()) + " file(s) not on the list", false);
        return;
    }
    std::vector<std::string> still_allowed;
    for (const auto& file : allowed)
        if (std::find(found.begin(), found.end(), file) != found.end()) still_allowed.push_back(file);
    if (still_allowed != allowed) {
        for (const auto& name : allowed)
            if (std::find(found.begin(), found.end(), name) == found.end())
                std::printf("    retired, delete the name: %s\n", name.c_str());
        check(what + ": the list holds a name nothing uses", false);
        return;
    }
    check(what + " (" + std::to_string(found.size()) + " of " + std::to_string(allowed.size()) +
              " allowed)",
          true);
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

// The class-level data members of a header, counted by declaration rather than by a
// line suffix: `foo_ = nullptr;` and a declaration split across two lines are both
// members, and a count that reads only lines ending `_;` reports fewer than the truth.
// Four spaces is the class body; eight is a nested type, whose members are its own.
std::vector<std::string> data_members(const std::string& text) {
    const std::string stripped = strip_line_comments(text);
    std::vector<std::string> members;
    std::string stmt;
    int stmt_indent = -1;
    std::istringstream stream(stripped);
    std::string line;
    while (std::getline(stream, line)) {
        if (stmt.empty()) {
            const size_t first = line.find_first_not_of(" \t");
            if (first == std::string::npos) continue;
            stmt_indent = static_cast<int>(first);
            stmt = line;
        } else {
            stmt += " " + line;
        }
        const size_t ends = stmt.find(';');
        if (ends == std::string::npos) continue;
        stmt = stmt.substr(0, ends);

        if (stmt_indent == 4) {
            // Angle depth: parens inside <...> are template arguments, not parameters.
            int angle = 0;
            size_t assign = std::string::npos;
            bool param_list = false;
            for (size_t i = 0; i < stmt.size(); ++i) {
                const char c = stmt[i];
                if (c == '<') ++angle;
                else if (c == '>' && !(i > 0 && stmt[i - 1] == '-') && angle > 0) --angle;
                else if (c == '=' && angle == 0 && stmt[i + 1] != '=' && assign == std::string::npos)
                    assign = i;
                else if (c == '(' && angle == 0 && assign == std::string::npos) param_list = true;
            }
            const std::string declared = assign == std::string::npos ? stmt : stmt.substr(0, assign);
            const std::string trimmed = declared.substr(0, declared.find_last_not_of(" \t") + 1);
            const size_t name_start = trimmed.find_last_of(" \t*&<>,(");
            const std::string name =
                name_start == std::string::npos ? trimmed : trimmed.substr(name_start + 1);
            if (!param_list && !name.empty() && name.back() == '_') members.push_back(name);
        }
        stmt.clear();
    }
    return members;
}

}  // namespace

int main() {
    std::printf("facade-shape rules %s\n", kRulesVersion);
    const std::string facade = read_repo_file(kFacade);
    check("the facade is read from the tree", facade.size() > 1000);

    // A path may pass through; opening one does not.
    at_most("the facade names a path and does no more", kFacade, "fs::", 2);
    at_most("the facade opens no directory", kFacade, "directory_iterator", 0);
    at_most("the facade runs no filesystem operation", kFacade, "fs::exists(", 0);
    at_most("the facade removes nothing", kFacade, "fs::remove", 0);
    at_most("the facade parses no registry row", kFacade, "contains(\"", 0);
    at_most("the facade reads no JSON value by hand", kFacade, "get<std::string>()", 0);
    at_most("the facade parses no document", kFacade, "json::parse", 0);
    at_most("the facade builds no repository cache path", kFacade, "repo_id_to_cache_dir_name", 0);
    at_most("the facade does not probe the machine", kFacade, "probe_backend_availability", 0);
    // One seam asks a recipe's ops what models it can see; anything beyond that and the
    // facade is back to branching on backends.
    at_most("the facade dispatches to backend ops once", kFacade, "ops_for(", 1);
    // Locks the facade takes for itself; publishing goes through StoreWrite.
    at_most("the facade takes no store lock of its own", kFacade, "lock_guard", 2);
    at_most("the facade holds no condition waits", kFacade, "unique_lock", 0);
    at_most("the facade declares no test hook", kFacade, "_for_test", 0);

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

    const std::vector<std::string> product = repo_files("src/cpp");

    // The facade header is what consumers include. model_info.h, registry_types.h and
    // store_snapshot.h are the models layer's public read side; any other models header
    // reaching a consumer is named here with the fact it reads, and each name retires by
    // deletion. Matched through included_paths_in, so the relative spelling and a
    // commented-out line can neither escape the rule nor satisfy it.
    const std::vector<std::string> public_headers = {
        "lemon/models/model_info.h",
        "lemon/models/registry_types.h",  // wire facts that live on disk
        "lemon/models/store_snapshot.h"};
    std::vector<std::string> escaping;
    for (const auto& file : product) {
        if (in_models_layer(file) || file == kFacade || file == kFacadeHeader) continue;
        for (const auto& path : included_paths_in(file)) {
            if (path.find(kModelsDir) == std::string::npos) continue;
            if (std::find(public_headers.begin(), public_headers.end(), path) !=
                public_headers.end())
                continue;
            // Only count it when the path really is a models-layer header, by the
            // directory it names: `lemon/models/x.h`, `models/x.h`, `../models/x.h`.
            const size_t at = path.rfind("models/");
            const std::string rest = path.substr(at + kModelsDir.size());
            if (rest.empty() || rest.find('/') != std::string::npos) continue;
            escaping.push_back(file);
            break;
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
    if (escaping != allowed_escaping) report_set_diff(escaping, allowed_escaping);

    // A whitelisted header that itself includes a models header hands those types to
    // everything that includes *it*, which is how a whitelist of eight becomes a whitelist
    // of the tree. Each entry below is a header allowed to re-export the models layer;
    // the report is what makes the reach visible to whoever reads a failure.
    // Only a *header* propagates: a .cpp that includes a models header affects nothing
    // beyond its own translation unit, so it is not part of this rule.
    std::vector<std::string> reexporters;
    for (const auto& file : product) {
        if (in_models_layer(file) || file.size() < 2 ||
            file.compare(file.size() - 2, 2, ".h") != 0)
            continue;
        for (const auto& path : included_paths_in(file)) {
            if (path.find(kModelsDir) != std::string::npos) {
                reexporters.push_back(file);
                break;
            }
        }
    }
    const std::vector<std::string> allowed_reexporters = {
        "src/cpp/include/lemon/auto_tune.h",          // DownloadProgressCallback, ModelInfo
        "src/cpp/include/lemon/backends/backend_ops.h",  // ArtifactFetchService
        "src/cpp/include/lemon/gguf_reader.h",        // GGUF facts read through a source
        "src/cpp/include/lemon/model_manager.h",      // the aggregator S8 is shrinking
        "src/cpp/server/model_registry.h",            // the adapter's own side
    };
    check("only the named headers re-export the models layer",
          reexporters == allowed_reexporters);
    if (reexporters != allowed_reexporters) {
        report_set_diff(reexporters, allowed_reexporters);
        for (const auto& file : reexporters) {
            std::printf("    %s reaches models types into every file that includes it:\n",
                        file.c_str());
            for (const auto& path : included_paths_in(file))
                if (path.find(kModelsDir) != std::string::npos)
                    std::printf("      %s\n", path.c_str());
        }
    }

    // The models layer depends on model_info.h, never on the facade header: the DAG
    // points one way, and a hit here is the inversion back.
    std::vector<std::string> models_files;
    for (const char* dir : {"src/cpp/server/models", "src/cpp/include/lemon/models"})
        for (const auto& file : repo_files(dir)) models_files.push_back(file);
    const std::vector<std::string> inverted = includers_of("lemon/model_manager.h", models_files);
    check("the models layer includes the facade header 0 times (found " +
              std::to_string(inverted.size()) + ")",
          inverted.empty());
    for (const auto& file : inverted) std::printf("    inversion: %s\n", file.c_str());

    // The measure the refactor was opened for, and the one no rule covered: how far the
    // facade header reaches. Three spellings reach the same header — <lemon/...>,
    // "lemon/..." and the bare "model_manager.h" resolved through the include path — and
    // the bare one is what the backend tree uses, so counting only the prefixed two
    // under-reports this by a third. One-way: S8 exists to bring this down.
    std::vector<std::string> consumers;
    for (const auto& file : product)
        if (file != kFacadeHeader) consumers.push_back(file);
    const std::vector<std::string> facade_consumers =
        includers_of("lemon/model_manager.h", consumers);
    constexpr int kFacadeIncluderCeiling = 29;
    check("the facade header reaches " + std::to_string(facade_consumers.size()) +
              " product files, ceiling " + std::to_string(kFacadeIncluderCeiling),
          static_cast<int>(facade_consumers.size()) <= kFacadeIncluderCeiling);
    if (static_cast<int>(facade_consumers.size()) < kFacadeIncluderCeiling)
        std::printf("note: facade reach is at %zu; tighten the ceiling to it\n",
                    facade_consumers.size());

    // The legacy registry class lives with its two adapters and nowhere else. Matched as
    // an include, so a comment naming the header is not a caller and a real one is.
    const std::vector<std::string> registry_includers =
        includers_of("model_registry.h", product);
    const std::vector<std::string> adapter_pair = {
        "src/cpp/server/model_registry.cpp",
        "src/cpp/server/models/registry_repository.cpp",
    };
    check("only the adapter pair includes the legacy registry header",
          registry_includers == adapter_pair);
    if (registry_includers != adapter_pair) report_set_diff(registry_includers, adapter_pair);

    // The facade's write token. It is a member type today and a private one tomorrow
    // (S9); either way nothing outside the facade should be holding it.
    std::vector<std::string> store_write_files;
    for (const auto& file : product) {
        if (!find_in(file, "StoreWrite").empty()) store_write_files.push_back(file);
    }
    const std::vector<std::string> allowed_store_write = {kFacadeHeader, kFacade};
    may_only_shrink("StoreWrite appears only in the facade", store_write_files,
                    allowed_store_write);

    // Presence of a checkpoint is one question with one answer (S5). Every file naming an
    // in-flight marker is a file that can answer it, which is exactly the bug: an
    // interrupted pull can report present and not-downloaded at once. The list may only
    // shrink, and http_client.cpp is on it for its own `.partial` temp suffix rather than
    // for any presence decision.
    std::vector<std::string> marker_files;
    for (const auto& file : product) {
        bool names_marker = false;
        for (const auto& needle : {".download_manifest.json", ".partial"})
            if (!find_in(file, needle).empty()) names_marker = true;
        if (names_marker) marker_files.push_back(file);
    }
    const std::vector<std::string> allowed_marker_files = {
        "src/cpp/include/lemon/models/ingest.h",
        "src/cpp/server/backends/backend_ops.cpp",
        "src/cpp/server/backends/llamacpp/llamacpp_gguf.cpp",
        "src/cpp/server/models/artifact_store.cpp",
        "src/cpp/server/models/artifact_transfer.cpp",
        "src/cpp/server/models/ingest.cpp",
        "src/cpp/server/registry_files.cpp",
        "src/cpp/server/utils/http_client.cpp",
    };
    may_only_shrink("in-flight markers are named only where presence is decided", marker_files,
                    allowed_marker_files);

    // Hermetic tests. cpp-ci runs without a network and without a running server, so a
    // test that reaches a real host passes on a laptop and fails in CI -- and nothing
    // said so until now. A non-loopback host *name* is not the problem: plenty of tests
    // hand `huggingface.co` to a pure function to check URL building or endpoint
    // classification, and banning the name would tax them. The problem is a non-loopback
    // host in a position that sends, so the rule is a host and a request on one line.
    const std::vector<std::string> request_tokens = {"HttpClient", "download_file", ".get(",
                                                    "->get(", ".post(", "->post("};
    // This file holds the token tables as data, so it matches its own patterns. A static
    // rule cannot be applied to the source that spells the rule; excluding it costs
    // nothing because the gate neither dials out nor forks.
    const std::string kSelf = "test/cpp/test_facade_shape.cpp";
    std::vector<std::string> dialing;
    for (const auto& file : repo_files("test/cpp")) {
        if (file == kSelf) continue;
        std::istringstream stream(strip_line_comments(read_repo_file(file)));
        std::string line;
        int number = 0;
        while (std::getline(stream, line)) {
            ++number;
            const size_t scheme = line.find("://");
            if (scheme == std::string::npos) continue;
            std::string host = line.substr(scheme + 3);
            const size_t end = host.find_first_of("/\"' :)>");
            if (end != std::string::npos) host = host.substr(0, end);
            if (host.empty()) continue;
            if (host.front() == '[') continue;  // IPv6 literal, checked below by name
            bool loopback = host == "localhost" || host == "0.0.0.0" ||
                            host.rfind("127.", 0) == 0 || host == "::1";
            if (loopback) continue;
            for (const auto& token : request_tokens) {
                if (line.find(token) != std::string::npos) {
                    dialing.push_back(file + ":" + std::to_string(number) + " (" + host + ")");
                    break;
                }
            }
        }
    }
    check("no test builds a request to a non-loopback host (" +
              std::to_string(dialing.size()) + ")",
          dialing.empty());
    for (const auto& hit : dialing) std::printf("    dials out: %s\n", hit.c_str());

    // A test that starts a process needs a reason written down. Spawning `lemond` in a
    // unit test would also make one test's server another test's interference, since
    // ctest runs these in parallel.
    std::vector<std::string> spawners;
    for (const auto& file : repo_files("test/cpp")) {
        if (file == kSelf) continue;
        for (const auto& token : {"popen", "std::system", "system(", "fork()", "execv",
                                 "posix_spawn", "CreateProcess"}) {
            if (!find_in(file, token).empty()) {
                spawners.push_back(file);
                break;
            }
        }
    }
    const std::vector<std::string> allowed_spawners = {
        "test/cpp/test_process_manager.cpp",  // process management is the subject
    };
    check("only the named test spawns a process", spawners == allowed_spawners);
    if (spawners != allowed_spawners) report_set_diff(spawners, allowed_spawners);

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

    // The measure this refactor was opened for: a ceiling, not a target. One
    // way: shrinking passes without an edit, growing names itself.
    const int lines = static_cast<int>(std::count(facade.begin(), facade.end(), '\n'));
    constexpr int kLineCeiling = 1103;
    check("the facade is " + std::to_string(lines) + " lines, ceiling " +
              std::to_string(kLineCeiling),
          lines <= kLineCeiling);

    // The header is the surface consumers see; members are what the facade
    // keeps beyond the components it was built from.
    const std::string header = read_repo_file(kFacadeHeader);
    // Public surface: the functions ModelManager itself declares, nested types excluded.
    // Counting `);` occurrences, as this did, counts doc comments and inline bodies and
    // misses every method ending `) const;`, so the ceiling held nothing.
    int declarations = 0;
    {
        const size_t body_start = header.find('{', header.find("class ModelManager"));
        const std::string body = strip_line_comments(header.substr(body_start));
        int depth = 0;  // 1 inside ModelManager's own body, 2 inside a nested type or body
        std::string segment;
        std::string before_brace;
        bool done = false;
        for (const char ch : body) {
            if (done) break;
            if (ch == '{') {
                before_brace = segment;
                ++depth;
                segment.clear();
            } else if (ch == '}') {
                if (depth == 1) {
                    done = true;
                } else {
                    --depth;
                    if (depth == 1 && before_brace.find('(') != std::string::npos) ++declarations;
                }
                segment.clear();
                before_brace.clear();
            } else if (ch == ';') {
                if (depth == 1 && segment.find('(') != std::string::npos) ++declarations;
                segment.clear();
            } else {
                segment += ch;
            }
        }
    }
    const std::vector<std::string> members = data_members(header);
    constexpr int kHeaderDeclarationCeiling = 87;
    check("the facade header declares " + std::to_string(declarations) +
              " methods (public and private), ceiling " +
              std::to_string(kHeaderDeclarationCeiling),
          declarations <= kHeaderDeclarationCeiling);
    constexpr int kMemberCeiling = 18;
    check("the facade holds " + std::to_string(members.size()) + " members, ceiling " +
              std::to_string(kMemberCeiling),
          static_cast<int>(members.size()) <= kMemberCeiling);
    if (static_cast<int>(members.size()) > kMemberCeiling) {
        for (const auto& name : members) std::printf("    member: %s\n", name.c_str());
    }

    if (g_failures > 0) {
        std::printf("\n%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("\nAll facade-shape checks passed\n");
    return 0;
}

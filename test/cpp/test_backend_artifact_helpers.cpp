#include "backend_typed_hook_registry_internal.h"
#include "lemon/backends/hf_cache_util.h"
#include "lemon/utils/audio_mime.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace hf_cache = lemon::backends::hf_cache;
int checks = 0;
int failures = 0;
namespace typed = lemon::backends::typed;
std::map<std::string, unsigned> entries_called;

void check(bool ok, const std::string& label) {
    ++checks;
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", label.c_str());
}

class CacheFixture {
public:
    CacheFixture() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        base = fs::temp_directory_path() / ("lemonade-artifact-" + std::to_string(stamp));
        if (!fs::create_directory(base)) throw std::runtime_error("Cannot create cache fixture");
    }
    ~CacheFixture() {
        std::error_code error;
        fs::remove_all(base, error);
    }
    fs::path cache(const std::string& name) {
        auto root = base / fs::u8path(name);
        fs::create_directories(root / "snapshots");
        return root;
    }
    void ref(const fs::path& root, const std::string& contents) {
        fs::create_directories(root / "refs");
        std::ofstream output(root / "refs/main", std::ios::binary);
        output << contents;
        output.close();
        if (!output) throw std::runtime_error("Cannot write cache ref fixture");
    }
    fs::path base;
};

void check_snapshots() {
    CacheFixture fixture;
    {
        auto root = fixture.cache("referenced");
        fs::create_directory(root / "snapshots/pinned-123");
        fs::create_directory(root / "snapshots/newer-unreferenced");
        fixture.ref(root, "pinned-123\n");
        check(hf_cache::active_snapshot_or_cache_path(root.u8string()) == (root / "snapshots/pinned-123").u8string(),
              "existing refs/main selects pinned snapshot despite another snapshot");
    }
    {
        auto root = fixture.cache("missing-ref");
        check(hf_cache::active_snapshot_or_cache_path(root.u8string()) == root.u8string(), "missing refs/main returns original cache");
    }
    {
        auto root = fixture.cache("unreferenced");
        fs::create_directory(root / "snapshots/unreferenced-456");
        check(hf_cache::active_snapshot_or_cache_path(root.u8string()) == root.u8string(), "unreferenced snapshot is never discovered by scanning");
    }
    {
        auto root = fixture.cache("empty-ref");
        fixture.ref(root, "");
        check(hf_cache::active_snapshot_or_cache_path(root.u8string()) == root.u8string(), "empty ref returns original cache");
        fixture.ref(root, " \t\r\n");
        check(hf_cache::active_snapshot_or_cache_path(root.u8string()) == root.u8string(), "whitespace-only ref returns original cache");
    }
    {
        auto root = fixture.cache("padded-ref");
        fs::create_directory(root / "snapshots/padded-789");
        fs::create_directory(root / "snapshots/ignored-second-line");
        fixture.ref(root, " \tpadded-789 \t\r\nignored-second-line\n");
        check(hf_cache::active_snapshot_or_cache_path(root.u8string()) == (root / "snapshots/padded-789").u8string(),
              "ref trims surrounding whitespace and reads only first line");
    }
    {
        auto root = fixture.cache("absent-snapshot");
        fixture.ref(root, "missing-000\n");
        fs::create_directory(root / "snapshots/other-111");
        check(hf_cache::active_snapshot_or_cache_path(root.u8string()) == root.u8string(), "absent referenced snapshot falls back even when another exists");
        const std::string original = root.u8string() + "/";
        check(hf_cache::active_snapshot_or_cache_path(original) == original, "fallback preserves original trailing separator byte for byte");
    }
    {
        auto root = fixture.cache(u8"模型-żółć");
        const auto snapshot = root / "snapshots" / fs::u8path(u8"wersja-音");
        fs::create_directory(snapshot);
        fixture.ref(root, u8"wersja-音\n");
        check(hf_cache::active_snapshot_or_cache_path(root.u8string()) == snapshot.u8string(), "Unicode cache and snapshot resolve as UTF-8");
        fixture.ref(root, "missing\n");
        const auto original = root.u8string() + "/";
        check(hf_cache::active_snapshot_or_cache_path(original) == original, "Unicode fallback preserves input bytes and trailing separator");
    }
    {
        auto root = fixture.cache("linked-snapshot");
        auto target = fixture.base / "snapshot-target";
        fs::create_directory(target);
        fixture.ref(root, "linked\n");
        std::error_code error;
        fs::create_directory_symlink(target, root / "snapshots/linked", error);
#ifdef _WIN32
        if (error) {
            std::printf("SKIP Windows HF symlink fixture: %s\n", error.message().c_str());
            return;
        }
#else
        if (error) throw std::runtime_error("Cannot create HF symlink fixture: " + error.message());
#endif
        check(hf_cache::exists(root / "snapshots/linked"), "HF existence mechanism accepts symlinked snapshot");
        check(hf_cache::active_snapshot_or_cache_path(root.u8string()) == (root / "snapshots/linked").u8string(),
              "symlinked snapshot returns lexical snapshot path without canonicalizing");
    }
}

void check_audio_mime() {
    const std::vector<std::pair<std::string_view, std::string>> cases = {
        {".mp3", "audio/mpeg"}, {".m4a", "audio/mp4"}, {".ogg", "audio/ogg"},
        {".flac", "audio/flac"}, {".webm", "audio/webm"}, {".wav", "audio/wav"},
        {"", "audio/wav"}, {".unknown", "audio/wav"}, {".MP3", "audio/wav"},
        {".FlAc", "audio/wav"}, {"mp3", "audio/wav"}, {".mp3 ", "audio/wav"},
    };
    for (const auto& [extension, mime] : cases) {
        check(lemon::utils::audio_mime_for_extension(extension) == mime, "literal case-sensitive MIME for '" + std::string(extension) + "'");
    }
    const std::string embedded = ".mp3tail";
    check(lemon::utils::audio_mime_for_extension(std::string_view(embedded.data(), 4)) == "audio/mpeg",
          "extension uses string_view length rather than null termination");
}

void artifact_interface(typed::ArtifactMetadataInput& metadata, typed::ArtifactFileInput& file,
    typed::ArtifactResolveInput& resolve, typed::ArtifactImportInput& imported, typed::ArtifactOps& ops) {
    auto& [handle, filename, model_type, labels] = metadata;
    auto& [file_handle, file_name, directory] = file;
    auto& [root, role, variant] = resolve;
    auto& [import_root] = imported;
    auto& [read_metadata, registration, validate_file, resolve_path, find_imported] = ops;
    static_assert(std::is_same_v<decltype(handle), typed::ArtifactHandle>);
    static_assert(std::is_same_v<decltype(filename), std::string>);
    static_assert(std::is_same_v<decltype(model_type), lemon::ModelType>);
    static_assert(std::is_same_v<decltype(labels), std::vector<std::string>>);
    static_assert(std::is_same_v<decltype(file_handle), typed::ArtifactHandle>);
    static_assert(std::is_same_v<decltype(file_name), std::string>);
    static_assert(std::is_same_v<decltype(directory), bool>);
    static_assert(std::is_same_v<decltype(root), typed::ArtifactHandle>);
    static_assert(std::is_same_v<decltype(role), std::string> && std::is_same_v<decltype(variant), std::string>);
    static_assert(std::is_same_v<decltype(import_root), typed::ArtifactHandle>);
    static_assert(std::is_same_v<decltype(read_metadata), typed::ArtifactMetadata (*)(const typed::ArtifactMetadataInput&, typed::ArtifactReadServices&)>);
    static_assert(std::is_same_v<decltype(registration), std::string (*)(const std::string&)>);
    static_assert(std::is_same_v<decltype(validate_file), std::string (*)(const typed::ArtifactFileInput&, typed::ArtifactReadServices&)>);
    static_assert(std::is_same_v<decltype(resolve_path), std::string (*)(const typed::ArtifactResolveInput&, typed::ArtifactReadServices&)>);
    static_assert(std::is_same_v<decltype(find_imported), std::string (*)(const typed::ArtifactImportInput&, typed::ArtifactReadServices&)>);
    static_assert(!std::is_default_constructible_v<typed::ArtifactHandle>);
    static_assert(!std::is_constructible_v<typed::ArtifactHandle, std::string>);
    static_assert(!std::is_constructible_v<typed::ArtifactHandle, std::size_t>);
    static_assert(!std::is_convertible_v<typed::ArtifactReadServices*, typed::SystemProbeServices*>);
    static_assert(!std::is_convertible_v<typed::ArtifactReadServices*, typed::EnvironmentServices*>);
}

class ConfinedReads final : public typed::ArtifactReadServices {
public:
    unsigned calls = 0;
    std::optional<typed::ArtifactHeader> metadata = typed::ArtifactHeader{"qwen3", 32768, {true, true, true}};
    std::string magic = "GGUF";
    std::vector<typed::ArtifactEntry> files;
    typed::ArtifactHandle root() const { return handle(0); }
    typed::ArtifactHandle file() const { return handle(1); }
    std::optional<typed::ArtifactHeader> header(typed::ArtifactHandle value) override {
        if (index(value) != 1) throw std::logic_error("not the approved model file");
        ++calls;
        return metadata;
    }
    std::string prefix(typed::ArtifactHandle value, std::size_t bytes) override {
        if (index(value) != 1 || bytes != 4) throw std::logic_error("only approved magic read is allowed");
        ++calls;
        return magic.substr(0, bytes);
    }
    std::vector<typed::ArtifactEntry> entries(typed::ArtifactHandle value) override {
        if (index(value) != 0) throw std::logic_error("not the approved root");
        ++calls;
        return files;
    }
    void add(std::string path, bool active = false, bool complete = true) {
        files.push_back({file(), path, "cache/" + path, active, complete});
    }
};

void check_typed_artifacts() {
    const auto& entries = typed::installed_targets();
    typed::audit_targets(entries);
    check(entries_called.empty(), "TypedAuditInvokesNothing: artifact table acquisition and audit");
    auto ops = std::get<typed::ArtifactOps>(entries.front().target);
    ConfinedReads reads;
    typed::ArtifactMetadataInput metadata{reads.file(), "MODEL.GGUF", lemon::ModelType::LLM, {"existing"}};
    typed::ArtifactFileInput file{reads.file(), "model.gguf"};
    typed::ArtifactResolveInput resolve{reads.root(), "main", "Q4_K_M"};
    typed::ArtifactImportInput imported{reads.root()};
    artifact_interface(metadata, file, resolve, imported, ops);
    check(true, "ArtifactAdaptersHaveNoAmbientRuntimeCapability: exact closed payloads and table");
    reads.metadata->block_count = 32;
    reads.metadata->head_count_kv_per_layer = {8, 8, 4};
    const auto result = ops.metadata(metadata, reads);
    check(result.header && result.header->context_length == 32768 && result.header->architecture == "qwen3" &&
        result.labels == std::vector<std::string>({"existing", "vision", "tool-calling", "mtp"}),
        "ArtifactOperationsActuallyWork: metadata and capability labels");
    check(result.header && result.header->block_count == 32 && result.header->head_count_kv_per_layer == std::vector<std::int64_t>({8, 8, 4}),
        "metadata retains KV dimensions and per-layer arrays");
    check(metadata.labels == std::vector<std::string>({"existing"}), "metadata input is unchanged");
    metadata.labels = result.labels;
    check(ops.metadata(metadata, reads).labels == result.labels, "metadata labels are not duplicated");
    for (const auto type : {lemon::ModelType::EMBEDDING, lemon::ModelType::RERANKING}) {
        metadata.model_type = type;
        metadata.labels = {"embedding"};
        check(ops.metadata(metadata, reads).labels == metadata.labels, "non-LLM metadata never reclassifies model");
    }
    reads.metadata.reset();
    check(!ops.metadata(metadata, reads).header, "metadata read failure returns no metadata");
    metadata.filename = "model.bin";
    const auto before = reads.calls;
    check(!ops.metadata(metadata, reads).header && reads.calls == before, "non-GGUF metadata obtains no read");
    check(ops.validate_registration("repo/model-GGUF").find("CHECKPOINT:VARIANT") != std::string::npos,
        "ArtifactOperationsActuallyWork: registration rejects missing quant variant");
    check(ops.validate_registration("repo/model-GGUF:Q4_K_M").empty() && ops.validate_registration("repo/model").empty(),
        "registration accepts variant and non-GGUF checkpoint");
    check(ops.validate_file(file, reads).empty(), "ArtifactOperationsActuallyWork: GGUF magic accepted");
    for (const auto& magic : {"GGU", "", "BAD!"}) {
        reads.magic = magic;
        check(ops.validate_file(file, reads) == "Invalid GGUF cache file", "truncated or corrupt GGUF rejected");
    }
    file.directory = true;
    const auto no_read = reads.calls;
    check(ops.validate_file(file, reads).empty() && reads.calls == no_read, "directory validation obtains no file read");
    file.directory = false;
    file.filename = "config.json";
    check(ops.validate_file(file, reads).empty() && reads.calls == no_read, "non-GGUF validation obtains no file read");
    reads.add("snapshots/old/Model-Q4_K_M.gguf");
    reads.add("snapshots/active/Model-Q8_0.gguf", true);
    reads.add("snapshots/active/mmproj-Q4_K_M.gguf", true);
    reads.add("snapshots/pending/AAA-Q4_K_M.gguf", false, false);
    reads.add("snapshots/active/model.gguf.partial", true);
    reads.add("../escape.gguf", true);
    check(ops.resolve_path(resolve, reads) == "cache/snapshots/old/Model-Q4_K_M.gguf",
        "ArtifactOperationsActuallyWork: quant resolves across completed snapshots");
    resolve.variant = "q8_0";
    check(ops.resolve_path(resolve, reads) == "cache/snapshots/active/Model-Q8_0.gguf", "case-insensitive quant prefers active snapshot");
    resolve.variant = "Model-Q4_K_M.gguf";
    check(ops.resolve_path(resolve, reads) == "cache/snapshots/old/Model-Q4_K_M.gguf", "exact GGUF filename resolves");
    resolve.variant = "model-q4_k_m.gguf";
    check(ops.resolve_path(resolve, reads).empty(), "exact filename remains case sensitive");
    resolve.variant = "Q2_K";
    check(ops.resolve_path(resolve, reads).empty(), "missing quant never falls back to wrong model");
    check(ops.find_imported(imported, reads) == "cache/snapshots/active/Model-Q8_0.gguf",
        "ArtifactOperationsActuallyWork: import selects active non-mmproj complete GGUF");
    reads.files.clear();
    reads.add("Q6_K/z-00002-of-00002.gguf");
    reads.add("Q6_K/z-00001-of-00002.gguf");
    resolve.variant = "Q6_K";
    check(ops.resolve_path(resolve, reads) == "cache/Q6_K/z-00001-of-00002.gguf", "folder shards select first sorted shard");
    reads.files.clear();
    reads.add("model-IQ4_XS-Q8nextn.gguf");
    resolve.variant = "IQ4_XS";
    check(ops.resolve_path(resolve, reads) == "cache/model-IQ4_XS-Q8nextn.gguf", "quant-token fallback uses shared variant semantics");
    reads.files.clear();
    reads.add("mmproj.gguf");
    reads.add("../escape.gguf");
    check(ops.find_imported(imported, reads).empty(), "import ignores mmproj and escaping paths");
    resolve.role = "mmproj";
    resolve.variant = "mmproj.gguf";
    check(ops.resolve_path(resolve, reads) == "cache/mmproj.gguf", "explicit auxiliary resolution retains mmproj");
    resolve.role = "draft";
    check(ops.resolve_path(resolve, reads) == "cache/mmproj.gguf", "auxiliary resolution uses exact supplied artifact name");
    ConfinedReads other;
    bool denied = false;
    try { ops.find_imported({other.root()}, reads); } catch (const std::logic_error&) { denied = true; }
    check(denied, "foreign service handle cannot authorize a read");
    for (const auto* entry : {"artifacts.metadata", "artifacts.validate_registration", "artifacts.validate_file",
        "artifacts.resolve_path", "artifacts.find_imported"}) {
        check(entries_called[entry] > 0, "TypedAuditInvokesNothing: all artifact member counters are live");
    }
    check(entries_called.size() == 5, "ArtifactAdaptersHaveNoAmbientRuntimeCapability: no other typed entry invoked");
}

}

void lemon::backends::typed::observe_entry(const char* entry) { ++entries_called[entry]; }

int main() {
    try {
        check_snapshots();
        check_audio_mime();
        check_typed_artifacts();
    } catch (const std::exception& error) {
        check(false, std::string("unexpected exception: ") + error.what());
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

#include "backend_runtime_services_internal.h"
#include "lemon/models/download_markers.h"
#include "lemon/utils/path_utils.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <regex>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lemon::backends::runtime {
namespace {
namespace fs = std::filesystem;
std::atomic<std::size_t> next_handle{1};
std::size_t handle_identity() {
    auto value = next_handle.load();
    do {
        if (value == std::numeric_limits<std::size_t>::max()) throw std::overflow_error("Artifact handle identity exhausted");
    } while (!next_handle.compare_exchange_weak(value, value + 1));
    return value;
}
bool within(const fs::path& path, const fs::path& root) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
}
class ReadFile final {
public:
    explicit ReadFile(const fs::path& path) {
#ifdef _WIN32
        handle_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        BY_HANDLE_FILE_INFORMATION info{};
        if (handle_ == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle_, &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
            if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
            throw std::runtime_error("Artifact is not a regular read handle");
        }
#else
        handle_ = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        struct stat info{};
        if (handle_ < 0 || fstat(handle_, &info) != 0 || !S_ISREG(info.st_mode)) {
            if (handle_ >= 0) close(handle_);
            throw std::runtime_error("Artifact is not a regular read handle");
        }
#endif
    }
    ~ReadFile() {
#ifdef _WIN32
        CloseHandle(handle_);
#else
        close(handle_);
#endif
    }
    ReadFile(const ReadFile&) = delete;
    ReadFile& operator=(const ReadFile&) = delete;
    std::string prefix(std::size_t bytes) {
        std::string result(bytes, '\0');
#ifdef _WIN32
        LARGE_INTEGER start{};
        DWORD read = 0;
        if (!SetFilePointerEx(handle_, start, nullptr, FILE_BEGIN) || !read_bytes(result.data(), static_cast<DWORD>(bytes), &read))
            throw std::runtime_error("Artifact prefix read failed");
        result.resize(read);
#else
        const auto read = pread(handle_, result.data(), bytes, 0);
        if (read < 0) throw std::runtime_error("Artifact prefix read failed");
        result.resize(static_cast<std::size_t>(read));
#endif
        return result;
    }
private:
#ifdef _WIN32
    bool read_bytes(char* bytes, DWORD size, DWORD* read) { return ::ReadFile(handle_, bytes, size, read, nullptr) != 0; }
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int handle_ = -1;
#endif
};
class HeaderCursor final {
public:
    explicit HeaderCursor(std::string bytes) : bytes_(std::move(bytes)) {}
    template<class T> T number() {
        if (sizeof(T) > bytes_.size() - position_) throw std::invalid_argument("Truncated bounded GGUF header");
        T value{};
        std::memcpy(&value, bytes_.data() + position_, sizeof(T));
        position_ += sizeof(T);
        return value;
    }
    std::string string() {
        const auto length = number<std::uint64_t>();
        if (length > bytes_.size() - position_) throw std::invalid_argument("Oversized bounded GGUF string");
        auto value = bytes_.substr(position_, static_cast<std::size_t>(length));
        position_ += static_cast<std::size_t>(length);
        return value;
    }
    std::int64_t integer(std::uint32_t type) {
        switch (type) {
            case 0: return number<std::uint8_t>();
            case 1: return number<std::int8_t>();
            case 2: return number<std::uint16_t>();
            case 3: return number<std::int16_t>();
            case 4: return number<std::uint32_t>();
            case 5: return number<std::int32_t>();
            case 7: return number<std::uint8_t>();
            case 10: {
                const auto value = number<std::uint64_t>();
                if (value > 1000000000) throw std::invalid_argument("Oversized bounded GGUF scalar");
                return static_cast<std::int64_t>(value);
            }
            case 11: return number<std::int64_t>();
            case 6: number<float>(); return 0;
            case 12: number<double>(); return 0;
            default: throw std::invalid_argument("Unknown bounded GGUF scalar");
        }
    }
private:
    std::string bytes_;
    std::size_t position_ = 0;
};
std::optional<typed::ArtifactHeader> read_header(const std::string& bytes) {
    if (bytes.size() < 4 || bytes.substr(0, 4) != "GGUF") return {};
    HeaderCursor cursor(bytes.substr(4));
    const auto version = cursor.number<std::uint32_t>();
    if (version != 2 && version != 3) throw std::invalid_argument("Unsupported bounded GGUF version");
    cursor.number<std::uint64_t>();
    const auto count = cursor.number<std::uint64_t>();
    if (count > 4096) throw std::length_error("GGUF header entry limit exceeded");
    typed::ArtifactHeader result;
    std::map<std::string, std::int64_t> scalars;
    for (std::uint64_t i = 0; i < count; ++i) {
        const auto key = cursor.string();
        const auto type = cursor.number<std::uint32_t>();
        if (type == 8) {
            const auto value = cursor.string();
            if (key == "general.architecture") result.architecture = value;
            else inspect_gguf_string(key, value, result.capabilities);
        } else if (type == 9) {
            const auto element = cursor.number<std::uint32_t>();
            const auto elements = cursor.number<std::uint64_t>();
            if (elements > 16384 || element == 9) throw std::length_error("GGUF header array limit exceeded");
            for (std::uint64_t j = 0; j < elements; ++j) {
                if (element == 8) inspect_gguf_string(key, cursor.string(), result.capabilities);
                else {
                    const auto value = cursor.integer(element);
                    if (value < 0 || value > 1000000000) throw std::invalid_argument("Oversized bounded GGUF array scalar");
                    if (key.size() >= 24 && key.substr(key.size() - 24) == ".attention.head_count_kv") result.head_count_kv_per_layer.push_back(value);
                    if (key.size() >= 23 && key.substr(key.size() - 23) == ".sliding_window_pattern") result.sliding_window_pattern.push_back(value != 0);
                }
            }
        } else {
            const auto value = cursor.integer(type);
            scalars[key] = value;
            if (key.find("nextn_predict_layers") != std::string::npos && value > 0) result.capabilities.mtp = true;
        }
    }
    const auto scalar = [&](const char* name) { const auto found = scalars.find(result.architecture + name); if (found == scalars.end()) return std::int64_t{0}; if (found->second < 0 || found->second > 1000000000) throw std::invalid_argument("Oversized bounded GGUF dimension"); return found->second; };
    result.context_length = scalar(".context_length");
    result.block_count = scalar(".block_count");
    result.embedding_length = scalar(".embedding_length");
    result.head_count_kv_scalar = scalar(".attention.head_count_kv");
    result.key_length = scalar(".attention.key_length");
    result.key_length_swa = scalar(".attention.key_length_swa");
    result.full_attention_interval = scalar(".full_attention_interval");
    if (result.head_count_kv_per_layer.empty() && result.head_count_kv_scalar && result.block_count) {
        if (result.block_count > 16384) throw std::length_error("GGUF layer limit exceeded");
        result.head_count_kv_per_layer.assign(static_cast<std::size_t>(result.block_count), result.head_count_kv_scalar);
    }
    for (const auto value : result.head_count_kv_per_layer) result.head_count_kv += value;
    result.swa_layer_count = static_cast<std::int64_t>(std::count(result.sliding_window_pattern.begin(), result.sliding_window_pattern.end(), true));
    return result;
}
}

struct ArtifactServices::Inventory {
    struct Entry {
        std::size_t token;
        fs::path path;
        fs::path target;
        std::string relative;
        bool active;
        std::unique_ptr<ReadFile> file;
    };
    std::vector<Entry> entries;
    bool directory;
    typed::ArtifactRootInfo info;
    Inventory(const std::string& approved_path, bool directory, const std::function<void(std::size_t)>& check) {
        check(1);
        if (approved_path.empty() || approved_path.find('\0') != std::string::npos) throw std::invalid_argument("Invalid approved artifact input");
        const auto root = fs::weakly_canonical(utils::path_from_utf8(approved_path));
        info = {utils::path_to_utf8(root), fs::exists(root), fs::exists(root / "snapshots"), false};
        this->directory = directory;
        entries.push_back({handle_identity(), root, root, "", false, directory || !info.exists ? nullptr : std::make_unique<ReadFile>(root)});
        if (!directory || !info.exists) return;
        if (!fs::is_directory(root)) throw std::invalid_argument("Artifact root is not a directory");
        std::string active;
        const auto ref = root / "refs" / "main";
        if (fs::is_directory(fs::symlink_status(root / "refs")) && fs::is_regular_file(fs::symlink_status(ref))) {
            ReadFile file(ref);
            active = file.prefix(256);
            while (!active.empty() && (active.back() == '\n' || active.back() == '\r')) active.pop_back();
            if (active.empty() || active == "." || active == ".." || active.find_first_of("/\\:") != std::string::npos || active.find('\0') != std::string::npos)
                throw std::invalid_argument("Invalid approved HF snapshot ref");
        }
        info.active_snapshot = !active.empty() && fs::exists(root / "snapshots" / active);
        fs::recursive_directory_iterator iterator(root), end;
        std::size_t visited = 0;
        for (; iterator != end; ++iterator) {
            check(1);
            if (++visited > 2048 || iterator.depth() > 16) throw std::length_error("Artifact inventory entry/depth limit exceeded");
            const auto path = iterator->path();
            const auto status = iterator->symlink_status();
            if (fs::is_directory(status)) continue;
            if (!fs::is_regular_file(status) && !fs::is_symlink(status)) throw std::invalid_argument("Nonregular artifact inventory entry");
            const auto relative = path.lexically_relative(root).generic_string();
            if (relative.size() > 4096) throw std::length_error("Artifact inventory path limit exceeded");
            check(relative.size());
            const auto target = fs::canonical(path);
            if (!within(target, root)) throw std::logic_error("Artifact link outside approved inventory");
            if (fs::is_symlink(status) && (relative.rfind("snapshots/", 0) != 0 || target.parent_path() != root / "blobs"))
                throw std::logic_error("Artifact link is not an approved HF snapshot/blob relationship");
            if (!fs::is_regular_file(target)) throw std::invalid_argument("Artifact link is not a regular file");
            entries.push_back({handle_identity(), path, target, relative,
                !active.empty() && relative.rfind("snapshots/" + active + "/", 0) == 0, std::make_unique<ReadFile>(target)});
        }
    }
};
ArtifactServices::ArtifactServices(InvocationGuard& guard, const std::string& approved_path, bool directory)
    : guard_(guard), lifetime_(guard.lifetime()), inventory_(std::make_unique<Inventory>(approved_path, directory,
        [this](std::size_t bytes) { check(bytes); })) {}

ArtifactServices::~ArtifactServices() = default;
void ArtifactServices::check(std::size_t bytes) {
    if (!invocation_active(lifetime_)) throw std::logic_error("Expired artifact invocation");
    guard_.check(typed::Effect::ArtifactRead, bytes);
}
std::size_t ArtifactServices::lookup(typed::ArtifactHandle file) {
    check();
    const auto token = index(file);
    const auto found = std::find_if(inventory_->entries.begin(), inventory_->entries.end(), [&](const auto& entry) { return entry.token == token; });
    if (found == inventory_->entries.end()) throw std::logic_error("Foreign or expired artifact handle");
    return static_cast<std::size_t>(found - inventory_->entries.begin());
}
typed::ArtifactHandle ArtifactServices::root() const { return handle(inventory_->entries.front().token); }
std::string ArtifactServices::prefix(typed::ArtifactHandle file, std::size_t bytes) {
    const auto at = lookup(file);
    if (bytes > 262144) throw std::length_error("Artifact prefix byte limit exceeded");
    check(bytes);
    const auto& read = inventory_->entries.at(at).file;
    if (!read) {
        if (!inventory_->info.exists) return "";
        throw std::logic_error("Artifact directory cannot be read as a file");
    }
    return read->prefix(bytes);
}
std::optional<typed::ArtifactHeader> ArtifactServices::header(typed::ArtifactHandle file) {
    const auto bytes = prefix(file, 262144);
    try { return read_header(bytes); }
    catch (const std::invalid_argument&) { return {}; }
    catch (const std::length_error&) { return {}; }
}
std::vector<typed::ArtifactEntry> ArtifactServices::entries(typed::ArtifactHandle root) {
    if (lookup(root) != 0 || !inventory_->directory) throw std::logic_error("Artifact handle is not the approved root");
    std::vector<typed::ArtifactEntry> result;
    for (std::size_t i = 1; i < inventory_->entries.size(); ++i) {
        const auto& entry = inventory_->entries[i];
        check(entry.relative.size());
        auto relative = entry.relative;
        bool committed = entry.relative.find(".incomplete") == std::string::npos;
        if (entry.relative.rfind("snapshots/", 0) == 0) {
            const auto end = entry.relative.find('/', 10);
            if (end != std::string::npos && !models::interrupted_download(utils::path_to_utf8(inventory_->entries.front().path / entry.relative.substr(0, end))).empty()) committed = false;
        }
        result.push_back({handle(entry.token), relative, utils::path_to_utf8(entry.path), entry.active,
            committed});
    }
    return result;
}
typed::ArtifactRootInfo ArtifactServices::root_info(typed::ArtifactHandle root) {
    if (lookup(root) != 0 || !inventory_->directory) throw std::logic_error("Artifact handle is not the approved root");
    return inventory_->info;
}
std::string ArtifactServices::approved_result(const std::string& path) {
    check(path.size());
    if (path.empty()) return "";
    for (const auto& entry : inventory_->entries) if (utils::path_to_utf8(entry.path) == path) {
        if (fs::weakly_canonical(entry.path) != entry.target) throw std::logic_error("Artifact inventory changed");
        return path;
    }
    throw std::logic_error("Returned artifact path outside approved handle inventory");
}
std::set<std::string> ArtifactServices::gguf_shards(const std::string& selected, const std::vector<std::string>& files) {
    const std::regex syntax("(.+)-([0-9]{5})-of-([0-9]{5})\\.gguf");
    std::smatch main;
    if (!std::regex_match(selected, main, syntax)) return {selected};
    const auto count = std::stoul(main[3]);
    std::set<std::string> shards;
    for (const auto& file : files) {
        std::smatch match;
        if (!std::regex_match(file, match, syntax) || match[1] != main[1] || match[3] != main[3]) continue;
        const auto index = std::stoul(match[2]);
        if (!index || index > count || !shards.insert(file).second) throw std::invalid_argument("Invalid GGUF shard inventory");
    }
    if (shards.size() != count || !shards.count(selected)) throw std::invalid_argument("Incomplete GGUF shard inventory");
    return shards;
}

std::vector<utils::ContainerMount> ArtifactServices::container_mounts(const std::string& approved_path,
    const std::string& expansion, const std::string& destination) {
    const bool directory = expansion == "directory_files";
    const auto target = fs::path(destination);
    if (approved_path.empty() || (expansion != "file" && expansion != "gguf_shards" && !directory) ||
        !target.is_absolute() || target.lexically_normal() != target || destination.rfind("/mnt/models/", 0) != 0)
        throw std::invalid_argument("Invalid container artifact mount");
    const auto requested = fs::absolute(utils::path_from_utf8(approved_path)).lexically_normal();
    const auto named = directory ? fs::canonical(requested) : fs::canonical(requested.parent_path()) / requested.filename();
    if (directory != fs::is_directory(named)) throw std::invalid_argument("Invalid container artifact kind");
    if (!directory && (expansion == "file" || !std::regex_match(named.filename().string(), std::regex("(.+)-[0-9]{5}-of-[0-9]{5}\\.gguf")))) {
        Inventory file_inventory(approved_path, false, [](std::size_t) {});
        if (!file_inventory.entries.front().file) throw std::invalid_argument("Missing approved artifact file");
        return {{utils::path_to_utf8(file_inventory.entries.front().target), destination}};
    }
    auto root = directory ? named : named.parent_path();
    for (auto parent = root; !parent.empty() && parent != parent.root_path(); parent = parent.parent_path()) {
        if (parent.filename() == "snapshots") { root = parent.parent_path(); break; }
    }
    Inventory inventory(utils::path_to_utf8(root), true, [](std::size_t) {});
    const auto canonical = fs::canonical(named);
    const auto found = std::find_if(inventory.entries.begin(), inventory.entries.end(), [&](const auto& entry) { return entry.path == named; });
    if (!directory && (found == inventory.entries.end() || found->target != canonical))
        throw std::logic_error("Mount source is outside the artifact inventory");
    std::vector<std::string> files;
    for (const auto& entry : inventory.entries) if (entry.file && entry.path.parent_path() == named.parent_path()) files.push_back(entry.path.filename().string());
    const auto shards = expansion == "gguf_shards" ? gguf_shards(named.filename().string(), files) : std::set<std::string>{};
    std::vector<utils::ContainerMount> mounts;
    for (const auto& entry : inventory.entries) {
        if (!entry.file) continue;
        bool selected = entry.path == named;
        fs::path target(destination);
        if (directory) {
            const auto relative = entry.path.lexically_relative(named);
            selected = !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
            target /= relative;
        } else if (!shards.empty()) {
            selected = entry.path.parent_path() == named.parent_path() && shards.count(entry.path.filename().string());
            target = target.parent_path() / entry.path.filename();
        }
        if (!selected) continue;
        if (fs::canonical(entry.path) != entry.target) throw std::logic_error("Artifact inventory changed");
        mounts.push_back({utils::path_to_utf8(entry.target), target.generic_string()});
    }
    std::sort(mounts.begin(), mounts.end(), [](const auto& a, const auto& b) { return a.destination < b.destination; });
    return mounts;
}

}

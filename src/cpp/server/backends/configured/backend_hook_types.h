#pragma once

#include "backend_hook_contracts_internal.h"
#include "lemon/gguf_capabilities.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace lemon::backends::typed {

enum class Platform { Windows, Linux, Macos };
struct BackendSelectionInput {
    std::string backend;
    std::string rocm_channel = "stable";
    std::vector<std::string> supported;
};
struct BackendSelection { std::string backend; bool use_gpu = false; };
struct DeviceValidationInput { std::string backend; std::string device; };
struct DeviceCheck { bool supported = false; std::string error; };
class BackendSelectionServices {
public:
    virtual ~BackendSelectionServices() = default;
    virtual bool supports(const std::string& backend) = 0;
    virtual DeviceCheck validate_device(const DeviceValidationInput& input) = 0;
};

enum class InstallPhase { Repository, Asset };
enum class InstallFact { TheRockVersion, RocmAssetFamily, CudaArchitecture };
struct InstallFactsInput {
    InstallPhase phase = InstallPhase::Repository;
    Platform platform = Platform::Linux;
    std::string backend;
    std::string requested_version;
    std::string pinned_version;
    std::string resolved_version;
    std::vector<InstallFact> requested;
    std::vector<InstallFact> repository_facts;
};
struct InstallFacts {
    std::optional<std::string> therock_version;
    std::optional<std::string> rocm_asset_family;
    std::optional<std::string> cuda_architecture;
};
class InstallFactsServices {
public:
    virtual ~InstallFactsServices() = default;
    virtual std::string therock_version() = 0;
    virtual std::string rocm_asset_family() = 0;
    virtual std::string cuda_architecture() = 0;
};

struct RuntimeOptionsInput { std::vector<std::string> labels; std::string draft_checkpoint; };
struct RuntimeFacts { bool dflash = false; bool mtp = false; };
struct PrepareInput {
    ModelType model_type = ModelType::LLM;
    bool hf_load = false;
    bool use_gpu = false;
    std::uint64_t context_size = 0;
    std::string main_path;
    std::string mmproj_path;
    std::string draft_path;
    std::string draft_checkpoint;
    std::vector<std::string> labels;
};
struct ModelFacts {
    bool hf_load = false;
    bool embedding = false;
    bool reranking = false;
    bool use_mmproj = false;
    bool mmproj_on_cpu = false;
    bool use_draft_checkpoint = false;
    std::uint64_t context_size = 0;
};

enum class ParentKey { Path, LlamaDevice, MetalNoResidency, Home, LibraryPath, CudaVisibleDevices, PrimeRenderOffload };
enum class RuntimeQuery { RocmArchitecture, TheRockDirectory, CudaVisibleDevices, HomeDirectory, CudaArchitecture };
enum class StagingScope { ManagedTheRock, BuiltinExternalTheRock };
class EnvironmentServices;
class StagingCapability final {
public:
    StagingCapability(const StagingCapability&) = default;
    StagingCapability& operator=(const StagingCapability&) = default;
private:
    StagingCapability(const EnvironmentServices* owner, StagingScope scope) : owner_(owner), scope_(scope) {}
    const EnvironmentServices* owner_;
    StagingScope scope_;
    friend class EnvironmentServices;
};
struct EnvironmentInput {
    Platform platform = Platform::Linux;
    std::string backend;
    std::string device;
    std::string executable_directory;
    std::optional<StagingCapability> staging;
};
struct StagingAction { StagingCapability capability; std::string architecture; };
struct EnvironmentPlan {
    std::vector<std::pair<std::string, std::string>> assignments;
    std::vector<StagingAction> staging;
};
class EnvironmentServices {
public:
    virtual ~EnvironmentServices() = default;
    virtual std::optional<std::string> parent(ParentKey key) = 0;
    virtual std::string runtime(RuntimeQuery query) = 0;
    bool owns(const StagingCapability& capability) const noexcept { return capability.owner_ == this; }
protected:
    StagingCapability staging_capability(StagingScope scope) const { return StagingCapability(this, scope); }
};

struct SystemVersionInput { std::string backend; std::string file_version; };
struct SystemInstallInput { Platform platform = Platform::Linux; std::string backend; bool binary_found = false; };
struct SystemInstallCheck { bool installed = false; std::string error; };
class SystemProbeServices {
public:
    virtual ~SystemProbeServices() = default;
    virtual std::string llama_version() = 0;
    virtual bool kfd_present() = 0;
    virtual bool hip_plugin_present() = 0;
};

class ArtifactReadServices;
class ArtifactHandle final {
public:
    ArtifactHandle(const ArtifactHandle&) = default;
    ArtifactHandle& operator=(const ArtifactHandle&) = default;
    bool operator==(const ArtifactHandle& other) const noexcept {
        return owner_ == other.owner_ && index_ == other.index_;
    }
private:
    ArtifactHandle(const ArtifactReadServices* owner, std::size_t index) : owner_(owner), index_(index) {}
    const ArtifactReadServices* owner_;
    std::size_t index_;
    friend class ArtifactReadServices;
};

struct ArtifactHeader {
    std::string architecture;
    std::int64_t context_length = 0;
    GgufCapabilities capabilities;
    std::int64_t block_count = 0;
    std::int64_t embedding_length = 0;
    std::int64_t head_count_kv = 0;
    std::int64_t key_length = 0;
    std::int64_t key_length_swa = 0;
    std::int64_t swa_layer_count = 0;
    std::int64_t full_attention_interval = 0;
    std::vector<std::int64_t> head_count_kv_per_layer;
    std::vector<bool> sliding_window_pattern;
    std::int64_t head_count_kv_scalar = 0;
};
struct ArtifactEntry {
    ArtifactHandle handle;
    std::string relative_path;
    std::string resolved_path;
    bool active = false;
    bool complete = true;
};
struct ArtifactMetadataInput {
    ArtifactHandle file;
    std::string filename;
    ModelType model_type = ModelType::LLM;
    std::vector<std::string> labels;
};
struct ArtifactMetadata {
    std::optional<ArtifactHeader> header;
    std::vector<std::string> labels;
};
struct ArtifactFileInput { ArtifactHandle file; std::string filename; bool directory = false; };
struct ArtifactResolveInput {
    ArtifactHandle root;
    std::string role = "main";
    std::string variant;
};
struct ArtifactImportInput { ArtifactHandle root; };
class ArtifactReadServices {
public:
    virtual ~ArtifactReadServices() = default;
    virtual std::optional<ArtifactHeader> header(ArtifactHandle file) = 0;
    virtual std::string prefix(ArtifactHandle file, std::size_t bytes) = 0;
    virtual std::vector<ArtifactEntry> entries(ArtifactHandle root) = 0;
protected:
    ArtifactHandle handle(std::size_t index) const { return ArtifactHandle(this, index); }
    std::size_t index(ArtifactHandle value) const {
        if (value.owner_ != this) throw std::logic_error("artifact read handle belongs to another service");
        return value.index_;
    }
};

struct BackendSelectionOps {
    BackendSelection (*select_backend)(const BackendSelectionInput&, BackendSelectionServices&);
    DeviceCheck (*validate_device)(const DeviceValidationInput&, BackendSelectionServices&);
};
using InstallContextHook = InstallFacts (*)(const InstallFactsInput&, InstallFactsServices&);
using RuntimeOptionsHook = RuntimeFacts (*)(const RuntimeOptionsInput&);
using PrepareHook = ModelFacts (*)(const PrepareInput&);
using EnvironmentHook = EnvironmentPlan (*)(const EnvironmentInput&, EnvironmentServices&);
struct SystemOps {
    std::string (*resolve_version)(const SystemVersionInput&, SystemProbeServices&);
    SystemInstallCheck (*check_install)(const SystemInstallInput&, SystemProbeServices&);
};
struct ArtifactOps {
    ArtifactMetadata (*metadata)(const ArtifactMetadataInput&, ArtifactReadServices&);
    std::string (*validate_registration)(const std::string&);
    std::string (*validate_file)(const ArtifactFileInput&, ArtifactReadServices&);
    std::string (*resolve_path)(const ArtifactResolveInput&, ArtifactReadServices&);
    std::string (*find_imported)(const ArtifactImportInput&, ArtifactReadServices&);
};
using RequestTransform = nlohmann::json (*)(nlohmann::json);
using ResponseTransform = nlohmann::json (*)(nlohmann::json, const nlohmann::json&, const std::string&);

}

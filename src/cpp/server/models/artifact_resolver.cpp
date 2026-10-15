#include <lemon/models/artifact_resolver.h>

#include <lemon/backends/backend_ops.h>
#include <lemon/model_manager.h>
#include <lemon/model_registry.h>
#include <lemon/models/checkpoint_grammar.h>
#include <lemon/models/fetchable_repository.h>

#include <optional>

namespace lemon {
namespace models {

namespace {

constexpr const char LOCAL_PATH[] = "local_path";
constexpr const char LOCAL_UPLOAD[] = "local_upload";
constexpr const char EXTRA_MODELS_DIR[] = "extra_models_dir";
constexpr const char FILE_SCHEME[] = "file://";

std::optional<LocalPathArtifactSource::Origin> origin_of(const std::string& source) {
    if (source == LOCAL_PATH) {
        return LocalPathArtifactSource::Origin::LocalPath;
    }
    if (source == LOCAL_UPLOAD) {
        return LocalPathArtifactSource::Origin::LocalUpload;
    }
    if (source == EXTRA_MODELS_DIR) {
        return LocalPathArtifactSource::Origin::ExtraModelsDir;
    }
    return std::nullopt;
}

ArtifactRef ref_for(const ModelInfo& info, const std::string& checkpoint_type, bool required) {
    ArtifactRef ref;
    ref.recipe = info.recipe;
    ref.checkpoint_type = checkpoint_type;
    ref.required = required;
    return ref;
}

const RegistryDescriptor* host_of_scheme_prefix(const std::string& checkpoint, RegistryId& id) {
    for (const auto& descriptor : all_registry_descriptors()) {
        const std::string prefix = descriptor.scheme_prefix + "://";
        if (checkpoint.rfind(prefix, 0) == 0) {
            id = descriptor.id;
            return &descriptor;
        }
    }
    return nullptr;
}

}  // namespace

ArtifactSourceResolver::ArtifactSourceResolver(Hooks hooks) : hooks_(std::move(hooks)) {}

FileValidator ArtifactSourceResolver::validator_for(const std::string& recipe) const {
    if (hooks_.validate_file == nullptr) {
        return nullptr;
    }
    return [this, recipe](const std::string& path) { return hooks_.validate_file(recipe, path); };
}

std::unique_ptr<ArtifactSource> ArtifactSourceResolver::open_repository(
    const RegistryId& host, const ArtifactRef& ref) const {
    std::unique_ptr<FetchableRepository> repository = hooks_.repository_for != nullptr
                                                            ? hooks_.repository_for(host, ref)
                                                            : create_repository(host, ref);
    if (repository == nullptr) {
        throw RegistrationError("no repository registered for host '" + host.str() + "'");
    }
    return repository;
}

void ArtifactSourceResolver::place(const ModelInfo& info, const std::string& checkpoint_type,
                                   ArtifactRef& ref) const {
    ref.layout = &layout_for(info, checkpoint_type);
    if (ref.layout != &hf_hub_layout()) {
        ref.sibling = info.resolved_path("main");
    }
}

const ArtifactLayout& ArtifactSourceResolver::layout_for(const ModelInfo& info,
                                                         const std::string& checkpoint_type) const {
    if (hooks_.checkpoint_layout != nullptr) {
        const ArtifactLayout* layout =
            hooks_.checkpoint_layout(info.recipe, CheckpointRef{info.model_name, checkpoint_type});
        if (layout != nullptr) {
            return *layout;
        }
    }
    return hf_hub_layout();
}

std::unique_ptr<ArtifactSource> ArtifactSourceResolver::source_for(
    const ModelInfo& info, const std::string& checkpoint_type) const {
    const std::string checkpoint = info.checkpoint(checkpoint_type);
    const bool required = hooks_.acquired_lazily != nullptr
                              ? !hooks_.acquired_lazily(checkpoint_type)
                              : checkpoint_type != "npu_cache";

    const backends::DownloadPolicy policy =
        hooks_.download_policy != nullptr ? hooks_.download_policy(info)
                                          : backends::DownloadPolicy::Repository;

    if (policy == backends::DownloadPolicy::None) {
        return std::make_unique<NoArtifactsSource>(ArtifactRef{});
    }

    if (policy == backends::DownloadPolicy::SelfManaged) {
        ArtifactRef ref = ref_for(info, checkpoint_type, required);
        ref.repo_id = checkpoint;
        return std::make_unique<SelfManagedArtifactSource>(std::move(ref), info.recipe);
    }

    if (const auto origin = origin_of(info.source)) {
        const std::string path =
            info.resolved_path(checkpoint_type).empty() ? checkpoint
                                                        : info.resolved_path(checkpoint_type);
        ArtifactRef ref = ref_for(info, checkpoint_type, required);
        ref.repo_id = path;
        return std::make_unique<LocalPathArtifactSource>(std::move(ref), path, *origin,
                                                         validator_for(info.recipe));
    }

    RegistryId scheme_host;
    if (host_of_scheme_prefix(checkpoint, scheme_host) != nullptr) {
        ArtifactRef ref = ref_for(info, checkpoint_type, required);
        ref.registry = scheme_host;
        ref.repo_id = checkpoint.substr(checkpoint.find("://") + 3);
        ref.variant = variant_from_checkpoint(ref.repo_id);
        ref.repo_id = repo_id_from_checkpoint(ref.repo_id);
        return open_repository(scheme_host, ref);
    }

    if (checkpoint.rfind(FILE_SCHEME, 0) == 0) {
        const std::string path = checkpoint.substr(sizeof(FILE_SCHEME) - 1);
        ArtifactRef ref = ref_for(info, checkpoint_type, required);
        ref.repo_id = path;
        return std::make_unique<LocalPathArtifactSource>(
            std::move(ref), path, LocalPathArtifactSource::Origin::LocalPath,
            validator_for(info.recipe));
    }

// Before the repo-id heuristic: it cannot tell a path from an org/repo.
    if (exists_on_disk(checkpoint)) {
        ArtifactRef ref = ref_for(info, checkpoint_type, required);
        ref.repo_id = checkpoint;
        return std::make_unique<LocalPathArtifactSource>(
            std::move(ref), checkpoint, LocalPathArtifactSource::Origin::LocalPath,
            validator_for(info.recipe));
    }

    if (checkpoint_looks_like_repo_id(checkpoint)) {
        ArtifactRef ref = ref_for(info, checkpoint_type, required);
        ref.repo_id = repo_id_from_checkpoint(checkpoint);
        ref.variant = variant_from_checkpoint(checkpoint);

// registry_id_from() reads an absent spelling as the default host, so ask first.
        std::optional<RegistryId> host;
        if (is_remote_registry_source(info.registry_source)) {
            host = registry_id_from(info.registry_source);
        } else if (is_remote_registry_source(info.source)) {
            host = registry_id_from(info.source);
        } else if (hooks_.default_registry_id != nullptr &&
                   !hooks_.default_registry_id().empty()) {
            host = registry_id_from(hooks_.default_registry_id());
        }
        if (host == std::nullopt) {
            throw RegistrationError("Model '" + info.model_name + "' checkpoint '" +
                                    checkpoint_type +
                                    "' names no host and no default registry is configured");
        }
        ref.registry = host;

        place(info, checkpoint_type, ref);
        return open_repository(*host, ref);
    }

    throw RegistrationError("Model '" + info.model_name + "' checkpoint '" + checkpoint_type +
                            "' binds to no artifact source: '" + checkpoint +
                            "' is neither a repository, a host scheme nor a file on disk");
}

}  // namespace models
}  // namespace lemon

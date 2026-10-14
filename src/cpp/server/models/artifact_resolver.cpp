#include <lemon/models/artifact_resolver.h>

#include <lemon/model_manager.h>
#include <lemon/model_registry.h>
#include <lemon/models/fetchable_repository.h>
#include <lemon/models/locator.h>

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

std::unique_ptr<ArtifactSource> ArtifactSourceResolver::source_for(
    const ModelInfo& info, const std::string& checkpoint_type) const {
    const std::string checkpoint = info.checkpoint(checkpoint_type);
    const bool required = hooks_.acquired_lazily != nullptr
                              ? !hooks_.acquired_lazily(checkpoint_type)
                              : checkpoint_type != "npu_cache";

    if (hooks_.fetches_no_artifacts != nullptr && hooks_.fetches_no_artifacts(info.recipe)) {
        return std::make_unique<NoArtifactsSource>(ArtifactRef{});
    }

    if (hooks_.backend_self_manages_downloads != nullptr &&
        hooks_.backend_self_manages_downloads(info.recipe)) {
        ArtifactRef ref;
        ref.repo_id = checkpoint;
        ref.required = required;
        return std::make_unique<SelfManagedArtifactSource>(std::move(ref), info.recipe);
    }

    if (const auto origin = origin_of(info.source)) {
        const std::string path =
            info.resolved_path(checkpoint_type).empty() ? checkpoint
                                                        : info.resolved_path(checkpoint_type);
        ArtifactRef ref;
        ref.repo_id = path;
        ref.required = required;
        return std::make_unique<LocalPathArtifactSource>(std::move(ref), path, *origin,
                                                         validator_for(info.recipe));
    }

    RegistryId scheme_host;
    if (host_of_scheme_prefix(checkpoint, scheme_host) != nullptr) {
        ArtifactRef ref;
        ref.registry = scheme_host;
        ref.repo_id = checkpoint.substr(checkpoint.find("://") + 3);
        ref.required = required;
        auto repository = hooks_.repository_for ? hooks_.repository_for(scheme_host, ref)
                                                 : nullptr;
        if (repository == nullptr) {
            throw RegistrationError("Model '" + info.model_name + "' checkpoint '" +
                                    checkpoint_type + "' names host '" + scheme_host.str() +
                                    "' with no repository registered for it");
        }
        return repository;
    }

    if (checkpoint.rfind(FILE_SCHEME, 0) == 0) {
        const std::string path = checkpoint.substr(sizeof(FILE_SCHEME) - 1);
        ArtifactRef ref;
        ref.repo_id = path;
        ref.required = required;
        return std::make_unique<LocalPathArtifactSource>(
            std::move(ref), path, LocalPathArtifactSource::Origin::LocalPath,
            validator_for(info.recipe));
    }

    // Before the repo-id heuristic, which tests only for '/' and so cannot tell an
    // absolute path from an org/repo.
    if (exists_on_disk(checkpoint)) {
        ArtifactRef ref;
        ref.repo_id = checkpoint;
        ref.required = required;
        return std::make_unique<LocalPathArtifactSource>(
            std::move(ref), checkpoint, LocalPathArtifactSource::Origin::LocalPath,
            validator_for(info.recipe));
    }

    if (checkpoint_looks_like_repo_id(checkpoint)) {
        ArtifactRef ref;
        ref.repo_id = repo_id_from_checkpoint(checkpoint);
        ref.variant = variant_from_checkpoint(checkpoint);
        ref.required = required;

        // An absent spelling means the default host to registry_id_from(), so ask
        // whether the entry names one before falling through to the config.
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

        auto repository = hooks_.repository_for ? hooks_.repository_for(*host, ref) : nullptr;
        if (repository == nullptr) {
            throw RegistrationError("Model '" + info.model_name + "' checkpoint '" +
                                    checkpoint_type + "' needs host '" + host->str() +
                                    "' with no repository registered for it");
        }
        return repository;
    }

    throw RegistrationError("Model '" + info.model_name + "' checkpoint '" + checkpoint_type +
                            "' binds to no artifact source: '" + checkpoint +
                            "' is neither a repository, a host scheme nor a file on disk");
}

}  // namespace models
}  // namespace lemon

#pragma once

#include "artifact_ref.h"
#include "artifact_transfer.h"
#include <lemon/utils/path_utils.h>

#include <filesystem>
#include <functional>
#include <string>

namespace lemon {
namespace models {

enum class ArtifactKind { Registry, Filesystem, Local, SelfManaged, None };

struct Presence {
    bool complete = false;
    std::string path;     // resolved location, empty when absent
    std::string reason;   // why incomplete
};

struct Acquisition {
    enum class Kind { Nothing, Plan, Delegated, NotApplicable } kind = Kind::Nothing;
    DownloadPlan plan;       // Kind::Plan
    std::string delegate;    // Kind::Delegated: who fetches it, for the error text
};

struct Deletion {
    // Deleting a checkpoint is polymorphic because a forgotten branch deletes a
    // user's own directory.
    enum class Kind { Nothing, CacheDir, Delegate } kind = Kind::Nothing;
    std::string reason;      // Delegate: the command the backend owns
};

// One checkpoint's answer to where its bytes are and what may be done with them.
// Constructed for a single checkpoint during a store build, so it lives with the
// generation that published it and a reader holding a source agrees with the
// snapshot it read.
class ArtifactSource {
  public:
    virtual ~ArtifactSource() = default;

    virtual ArtifactKind kind() const = 0;
    const ArtifactRef& ref() const { return ref_; }

    virtual std::string resolve() const = 0;
    virtual Presence present() const = 0;
    virtual Acquisition acquisition() const = 0;
    virtual Deletion deletion() const = 0;

  protected:
    explicit ArtifactSource(ArtifactRef ref) : ref_(std::move(ref)) {}

    ArtifactRef ref_;
};

// Non-empty return means the file is not usable.
using FileValidator = std::function<std::string(const std::string& resolved_path)>;

inline bool exists_on_disk(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    return std::filesystem::exists(utils::path_from_utf8(path), ec);
}

// Bytes found on disk; the origin decides whether deletion may remove them.
class LocalPathArtifactSource : public ArtifactSource {
  public:
    enum class Origin { ExtraModelsDir, LocalPath, LocalUpload };

    LocalPathArtifactSource(ArtifactRef ref, std::string path, Origin origin,
                            FileValidator validator = nullptr)
        : ArtifactSource(std::move(ref)), path_(std::move(path)), origin_(origin),
          validator_(std::move(validator)) {}

    ArtifactKind kind() const override { return ArtifactKind::Local; }
    std::string resolve() const override { return path_; }

    Presence present() const override {
        if (!exists_on_disk(path_)) {
            return Presence{false, "", "no file at " + path_};
        }
        if (validator_ != nullptr) {
            const std::string reason = validator_(path_);
            if (!reason.empty()) {
                return Presence{false, path_, reason};
            }
        }
        return Presence{true, path_, ""};
    }

    Acquisition acquisition() const override { return Acquisition{}; }

    Deletion deletion() const override {
        if (origin_ == Origin::LocalUpload) {
            return Deletion{Deletion::Kind::CacheDir, ""};
        }
        return Deletion{Deletion::Kind::Nothing, ""};
    }

  private:
    std::string path_;
    Origin origin_;
    FileValidator validator_;
};

// A backend fetches and removes its own weights.
class SelfManagedArtifactSource : public ArtifactSource {
  public:
    using PresenceProbe = std::function<Presence(const ArtifactRef& ref)>;

    SelfManagedArtifactSource(ArtifactRef ref, std::string delegate, PresenceProbe probe = nullptr)
        : ArtifactSource(std::move(ref)), delegate_(std::move(delegate)),
          probe_(std::move(probe)) {}

    ArtifactKind kind() const override { return ArtifactKind::SelfManaged; }
    std::string resolve() const override { return probe_ != nullptr ? probe_(ref_).path : ""; }

    Presence present() const override {
        if (probe_ == nullptr) {
            return Presence{false, "", "no presence probe for " + delegate_};
        }
        return probe_(ref_);
    }

    Acquisition acquisition() const override {
        return Acquisition{Acquisition::Kind::Delegated, DownloadPlan{}, delegate_};
    }

    Deletion deletion() const override { return Deletion{Deletion::Kind::Delegate, delegate_}; }

  private:
    std::string delegate_;
    PresenceProbe probe_;
};

// A route with no bytes, such as a cloud model.
class NoArtifactsSource : public ArtifactSource {
  public:
    explicit NoArtifactsSource(ArtifactRef ref) : ArtifactSource(std::move(ref)) {}

    ArtifactKind kind() const override { return ArtifactKind::None; }
    std::string resolve() const override { return ""; }
    Presence present() const override { return Presence{true, "", ""}; }
    Acquisition acquisition() const override {
        return Acquisition{Acquisition::Kind::NotApplicable, DownloadPlan{}, ""};
    }
    Deletion deletion() const override { return Deletion{Deletion::Kind::Nothing, ""}; }
};

}  // namespace models
}  // namespace lemon

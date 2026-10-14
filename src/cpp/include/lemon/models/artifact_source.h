#pragma once

#include "artifact_ref.h"
#include "artifact_transfer.h"

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

}  // namespace models
}  // namespace lemon

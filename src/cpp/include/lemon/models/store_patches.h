#pragma once

#include <map>
#include <string>

namespace lemon {

struct ModelInfo;

namespace models {

class ModelArtifactStore;
class ModelStore;

// Bringing the published set in line with one thing that changed. A patch reads disk
// against the published view and takes the writer only to move the rows it changed.
class StorePatches {
  public:
    struct Deps {
        ModelStore* store;
        ModelArtifactStore* artifacts;
    };

    explicit StorePatches(Deps deps) : deps_(deps) {}

    // A model's bytes appeared or went away; collections routing through it follow.
    void download_status(const std::string& model_name, bool downloaded);

    // Gone rather than misplaced: a registration leaves the set, a built-in stays.
    void removed(const std::string& model_name);

    // Onto the caller's write scope, so a saved file and its row are never apart.
    static void options(std::map<std::string, ModelInfo>& draft, const ModelInfo& info);

  private:
    Deps deps_;
};

}  // namespace models
}  // namespace lemon

#pragma once

#include <map>
#include <string>

namespace lemon {

struct ModelInfo;

namespace models {

class ModelArtifactStore;
class ModelStore;

// Bringing the published set in line with one thing that changed, instead of composing
// it again. A patch that has to look at disk does it against the published view and takes
// the store's writer only to move the rows it changed, so a download never waits on a
// rebuild and a rebuild never publishes over one.
class StorePatches {
  public:
    struct Deps {
        ModelStore* store;
        ModelArtifactStore* artifacts;
    };

    explicit StorePatches(Deps deps) : deps_(deps) {}

    // A model's bytes appeared or went away. Collections that route through it follow
    // rather than wait for a rebuild.
    void download_status(const std::string& model_name, bool downloaded);

    // Its files are gone rather than misplaced: a registration leaves the set, since no
    // catalog would list it again, and a built-in stays and says it is not here.
    void removed(const std::string& model_name);

    // A re-layered option set onto the live row, inside a write scope the caller holds
    // so the file it saved and the row it describes cannot be separated.
    static void options(std::map<std::string, ModelInfo>& draft, const ModelInfo& info);

  private:
    Deps deps_;
};

}  // namespace models
}  // namespace lemon

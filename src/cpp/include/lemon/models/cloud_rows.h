#pragma once

#include <lemon/models/store.h>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace lemon {

struct ModelInfo;

namespace models {

// The rows a cloud provider's listing put in the store. A build picks the listing up by
// asking the provider; these are for the moments that cannot wait for a build — a client
// added or removed a key, or asked what a provider has right now.
class CloudRows {
public:
    // The provider's current listing. It must not throw: no key, a plaintext-only policy
    // or an upstream failure all mean "nothing listed".
    using List = std::function<std::vector<ModelInfo>(const std::string& provider)>;

    CloudRows(ModelStore& store, List list);

    // Reseeds one provider: what it stopped listing disappears, other providers are
    // untouched. Returns how many rows it holds.
    std::size_t refresh(const std::string& provider);

    // Drops one provider's rows, leaving every other source alone. Returns how many went.
    std::size_t evict(const std::string& provider);

    std::size_t count(const std::string& provider) const;

private:
    ModelStore& store_;
    List list_;
};

}  // namespace models
}  // namespace lemon

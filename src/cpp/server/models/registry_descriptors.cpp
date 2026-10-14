#include <lemon/models/registry_descriptors.h>

#include <lemon/models/fetchable_repository.h>

namespace lemon {
namespace models {

namespace builtin_registries {

RegistryDescriptor huggingface() {
    return RegistryDescriptor{RegistryId("huggingface"),
                              {"huggingface", "hf", ""},
                              "Hugging Face",
                              "hf",
                              "models--",
                              "main",
                              "https://huggingface.co"};
}

RegistryDescriptor modelscope() {
    return RegistryDescriptor{RegistryId("modelscope"),
                              {"modelscope", "ms"},
                              "ModelScope",
                              "ms",
                              "modelscope--models--",
                              "master",
                              ""};
}

}  // namespace builtin_registries

// Host identity, without any ability to fetch from it: a client that only parses
// and names checkpoints links this instead of a repository implementation.
LEMONADE_REGISTER_REGISTRY_DESCRIPTOR(builtin_huggingface, builtin_registries::huggingface());
LEMONADE_REGISTER_REGISTRY_DESCRIPTOR(builtin_modelscope, builtin_registries::modelscope());

}  // namespace models
}  // namespace lemon

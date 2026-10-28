#include <lemon/models/registry_descriptors.h>

#include <lemon/models/fetchable_repository.h>

namespace lemon {
namespace models {

namespace builtin_registries {

RegistryDescriptor huggingface() {
    RegistryDescriptor d;
    d.id = RegistryId("huggingface");
    d.aliases = {"huggingface", "hf", ""};
    d.display_name = "Hugging Face";
    d.scheme_prefix = "hf";
    d.cache_dir_prefix = "models--";
    d.default_revision = "main";
    d.endpoint_default = "https://huggingface.co";
    d.download_protocol = download_protocols::hf_hub;
    d.doc = DocEntry{
        "huggingface",
        "Hugging Face",
        "The default remote registry: GGUF repositories over HTTPS.",
        {"A token-gated repository needs `HF_TOKEN`.",
         "`HF_ENDPOINT` moves the API to a mirror or gateway; the cache layout does not change."},
        "https://huggingface.co/{repo}"};
    d.env = {EnvVar{"HF_TOKEN", "read access to gated repositories"},
             EnvVar{"HF_ENDPOINT", "a mirror or gateway for the API"}};
    d.revision_semantics = "branch, tag or commit SHA";
    d.staleness_basis = "the commit SHA the API reports for the revision";
    d.supports_search = true;
    return d;
}

RegistryDescriptor modelscope() {
    RegistryDescriptor d;
    d.id = RegistryId("modelscope");
    d.aliases = {"modelscope", "ms"};
    d.display_name = "ModelScope";
    d.scheme_prefix = "ms";
    d.cache_dir_prefix = "modelscope--models--";
    d.default_revision = "master";
    d.download_protocol = download_protocols::modelscope_api;
    d.doc = DocEntry{
        "modelscope",
        "ModelScope",
        "A second remote registry, reached through the provider's own file API.",
        {"A token needs `MODELSCOPE_API_TOKEN`, or `MODELSCOPE_ACCESS_TOKEN`.",
         "`MODELSCOPE_ENDPOINT` moves the API to a mirror; the default is "
         "`https://modelscope.cn`.",
         "The download endpoint is the provider API, so no separate file host is configured."},
        "https://modelscope.cn/models/{repo}"};
    d.env = {EnvVar{"MODELSCOPE_API_TOKEN", "read access to gated repositories"},
             EnvVar{"MODELSCOPE_ACCESS_TOKEN", "an accepted alternative to the API token"},
             EnvVar{"MODELSCOPE_ENDPOINT", "a mirror or gateway for the API"}};
    d.revision_semantics = "branch, tag or commit SHA";
    d.staleness_basis = "a content hash over the resolved file tree";
    d.supports_search = true;
    return d;
}

}  // namespace builtin_registries

// Identity only, no fetch capability.
LEMONADE_REGISTER_REGISTRY_DESCRIPTOR(builtin_huggingface, builtin_registries::huggingface());
LEMONADE_REGISTER_REGISTRY_DESCRIPTOR(builtin_modelscope, builtin_registries::modelscope());

}  // namespace models
}  // namespace lemon

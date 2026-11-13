#include "backend_definition_exports_internal.h"

#include <stdexcept>

namespace lemon::backends {

const BuiltinContainerExecutionOwner& reviewed_gufo_execution_owner() {
    static const BuiltinContainerExecutionOwner owner{{"gufo", 1},
        nlohmann::json::parse(R"owner({"address":{"source":"process_start"},"artifacts":{"draft":{"missing_error":"Optional model file is missing","required_when":false,"role":"draft","strategy":"resolved_path"},"main":{"missing_error":{"concat":["GGUF file not found for checkpoint: ",{"ref":"/model/checkpoint"}]},"required_when":true,"role":"main","strategy":"resolved_path"},"mmproj":{"missing_error":"Optional model file is missing","required_when":false,"role":"mmproj","strategy":"resolved_path"}},"backend":{"fixed":"rocm"},"confinement":"lemonade-isolated-v1","containers":{"rocm":{"cap_add":[],"devices":["/dev/dri","/dev/kfd"],"network":"private_internal","pin":{"backend":"rocm","recipe":"gufo","source":"backend_versions"},"policy":"lemonade-isolated-v1","repository":"docker.io/kyuz0/amd-strix-halo-toolboxes","stop_grace_seconds":10,"teardown":"stop_remove_reap"}},"environment":{"order":"preset_first","preset":"inherit","set":[]},"execution":{"cases":[{"backends":["rocm"],"container":"rocm","id":"rocm-container","kind":"container","program":{"name":"gufo","source":"image_command"}}]},"install":{"cases":[],"strategy":"oci"},"kind":"container","mounts":[{"artifact":"main","destination":"/mnt/models","expand":"gguf_shards","id":"main","layout":"basename_numbered"},{"artifact":"mmproj","destination":"/mnt/models","expand":"gguf_shards","id":"mmproj","layout":"basename_numbered","when":{"not":{"eq":[{"ref":"/artifacts/mmproj"},""]}}},{"artifact":"draft","destination":"/mnt/models","expand":"gguf_shards","id":"draft","layout":"basename_numbered","when":{"not":{"eq":[{"ref":"/artifacts/draft"},""]}}}],"protected_environment":["HOME","LD_PRELOAD","LD_LIBRARY_PATH","PATH","HIP_VISIBLE_DEVICES","ROCR_VISIBLE_DEVICES","CUDA_VISIBLE_DEVICES"],"recipe":"gufo","working_directory":"inherit"})owner"),
        "sha256:9d49f5bc516ae6de035e05906125954b520de367758883bca753289f0d7e463b"};
    return owner;
}

const BuiltinContainerExecutionOwner& reviewed_container_execution_owner(const std::string& id) {
#include "dormant_container_owners.h"
    throw std::invalid_argument("Unknown dormant container execution owner: " + id);
}

}

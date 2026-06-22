#include "lemon/backends/backend_ops.h"

namespace lemon {
namespace backends {

const BackendOps* default_backend_ops() {
    static const BackendOps kDefault;
    return &kDefault;
}

} // namespace backends
} // namespace lemon

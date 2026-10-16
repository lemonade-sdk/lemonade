#include "require_llm.h"

#include <lemon/utils/aixlog.hpp>

#include "lemon/model_types.h"
#include "lemon/router.h"

namespace lemon {

const RouteError kNotLlm{400, "", "`model` is not an LLM, which is checked after it loads"};

bool require_llm(Router& router, const std::string& model, const std::string& operation,
                 httplib::Response& res) {
    if (router.get_model_type(model) == ModelType::LLM) {
        return true;
    }
    LOG(ERROR, "Server") << "Model does not support " << operation << std::endl;
    write_openai_error(res, kNotLlm, "This model does not support " + operation +
                                         ". Only LLM models support this endpoint.");
    return false;
}

} // namespace lemon

#include "unsupported.h"

namespace lemon {

const RouteError OllamaUnsupportedRoute::kNotSupported{
    501, "", "Lemonade does not support this Ollama operation"};

void OllamaUnsupportedRoute::handle(RouteRequest&, httplib::Response& res) {
    write_plain_error(res, kNotSupported, "not supported by Lemonade");
}

} // namespace lemon

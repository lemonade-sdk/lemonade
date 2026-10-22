#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace amica_engine {

class Bundle;
class ChatFrontend;

struct Generation {
    std::vector<std::int64_t> ids;  // includes the stop token that ended it
    double ttft_ms = 0.0;
    double decode_ms = 0.0;
    double tokens_per_second = 0.0;
};

// The model the HTTP API generates with.
class Generator {
public:
    virtual ~Generator() = default;

    // Greedy generation. on_token sees every generated token; returning false
    // stops generation, for example when the client has disconnected.
    virtual Generation generate(const std::vector<std::int64_t>& prompt, std::size_t max_new_tokens,
                                const std::set<std::int64_t>& stop_ids,
                                const std::function<bool(std::int64_t)>& on_token) = 0;

    virtual bool device_removed() = 0;
};

// Serves /v1/chat/completions, /v1/completions, /v1/models and /health until
// the process exits. Returns non-zero when the port cannot be bound.
int serve(const std::string& host, int port, const std::string& model_name, const Bundle& bundle,
          const ChatFrontend& frontend, Generator& generator);

}  // namespace amica_engine

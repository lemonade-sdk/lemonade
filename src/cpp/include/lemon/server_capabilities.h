#pragma once

#include <nlohmann/json.hpp>

namespace lemon {

using json = nlohmann::json;

// Base capability interface
class ICapability {
public:
    virtual ~ICapability() = default;
};

// Core completion capabilities that all servers must support
class ICompletionServer : public virtual ICapability {
public:
    virtual ~ICompletionServer() = default;
    virtual json chat_completion(const json& request) = 0;
    virtual json completion(const json& request) = 0;
};

// Optional embeddings capability
class IEmbeddingsServer : public virtual ICapability {
public:
    virtual ~IEmbeddingsServer() = default;
    virtual json embeddings(const json& request) = 0;
};

// Optional reranking capability
class IRerankingServer : public virtual ICapability {
public:
    virtual ~IRerankingServer() = default;
    virtual json reranking(const json& request) = 0;
};

// Helper to check if a server supports a capability
template<typename T>
bool supports_capability(ICapability* server) {
    return dynamic_cast<T*>(server) != nullptr;
}

} // namespace lemon

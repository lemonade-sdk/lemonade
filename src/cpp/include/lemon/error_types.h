#pragma once

#include <string>
#include <exception>
#include <nlohmann/json.hpp>

namespace lemon {

using json = nlohmann::json;

// Error types as constants
namespace ErrorType {
    constexpr const char* MODEL_NOT_LOADED = "model_not_loaded";
    constexpr const char* MODEL_INVALIDATED = "model_invalidated";
    constexpr const char* BACKEND_ERROR = "backend_error";
    constexpr const char* NETWORK_ERROR = "network_error";
    constexpr const char* INVALID_REQUEST = "invalid_request";
    constexpr const char* UNSUPPORTED_OPERATION = "unsupported_operation";
    constexpr const char* INSTALLATION_ERROR = "installation_error";
    constexpr const char* DOWNLOAD_ERROR = "download_error";
    constexpr const char* PROCESS_ERROR = "process_error";
    constexpr const char* FILE_ERROR = "file_error";
    constexpr const char* INTERNAL_ERROR = "internal_error";
}

// Base exception class for all Lemon errors
class LemonException : public std::exception {
public:
    LemonException(const std::string& message, const std::string& type = ErrorType::INTERNAL_ERROR)
        : message_(message), type_(type) {}

    const char* what() const noexcept override {
        return message_.c_str();
    }

    const std::string& type() const { return type_; }

    json to_json() const {
        return {
            {"error", {
                {"message", message_},
                {"type", type_}
            }}
        };
    }

protected:
    std::string message_;
    std::string type_;
};

// Specific exception types
class ModelNotLoadedException : public LemonException {
public:
    ModelNotLoadedException(const std::string& details = "")
        : LemonException("No model loaded" + (details.empty() ? "" : ": " + details),
                        ErrorType::MODEL_NOT_LOADED) {}
};

class BackendException : public LemonException {
public:
    BackendException(const std::string& backend, const std::string& message, int status_code = 0)
        : LemonException(backend + " error: " + message, ErrorType::BACKEND_ERROR),
          backend_(backend), status_code_(status_code) {}

    json to_json() const {
        auto j = LemonException::to_json();
        j["error"]["backend"] = backend_;
        if (status_code_ > 0) {
            j["error"]["status_code"] = status_code_;
        }
        return j;
    }

private:
    std::string backend_;
    int status_code_;
};

class NetworkException : public LemonException {
public:
    NetworkException(const std::string& message)
        : LemonException("Network error: " + message, ErrorType::NETWORK_ERROR) {}
};

class InvalidRequestException : public LemonException {
public:
    InvalidRequestException(const std::string& message)
        : LemonException("Invalid request: " + message, ErrorType::INVALID_REQUEST) {}
};

class UnsupportedOperationException : public LemonException {
public:
    UnsupportedOperationException(const std::string& operation, const std::string& backend = "")
        : LemonException(operation + " not supported" + (backend.empty() ? "" : " by " + backend),
                        ErrorType::UNSUPPORTED_OPERATION) {}
};

class ModelInvalidatedException : public LemonException {
public:
    ModelInvalidatedException(const std::string& model_name, const std::string& reason = "")
        : LemonException("Model '" + model_name + "' was invalidated" +
                        (reason.empty() ? "" : ": " + reason) +
                        ". Please download the model again.",
                        ErrorType::MODEL_INVALIDATED) {}
};

// Helper class for consistent error responses
class ErrorResponse {
public:
    static json create(const std::string& message,
                      const std::string& type = ErrorType::INTERNAL_ERROR,
                      const json& details = {}) {
        json error = {
            {"error", {
                {"message", message},
                {"type", type}
            }}
        };

        if (!details.empty()) {
            error["error"]["details"] = details;
        }

        return error;
    }

    static json from_exception(const LemonException& e) {
        return e.to_json();
    }

    static json from_std_exception(const std::exception& e,
                                   const std::string& type = ErrorType::INTERNAL_ERROR) {
        return create(e.what(), type);
    }
};

} // namespace lemon

#include <lemon/models/catalog_locator.h>

#include <lemon/utils/aixlog.hpp>
#include <lemon/utils/http_client.h>
#include <lemon/utils/path_utils.h>
#include <lemon/utils/safe_fs.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;

namespace lemon {
namespace models {

namespace {

constexpr const char* kLog = "CatalogLocator";

bool read_body(const fs::path& path, std::string& body, CatalogState& observed) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    body = buffer.str();

    observed.present = true;
    observed.size = body.size();
    observed.modified = fs::last_write_time(path, ec).time_since_epoch().count();
    return true;
}

nlohmann::json parse_document(const std::string& id, const std::string& body) {
    nlohmann::json document = nlohmann::json::parse(body);
    if (!document.is_object()) {
        throw std::runtime_error("Catalog '" + id + "' is not a JSON object of model rows");
    }
    return document;
}

class FileCatalogLocator final : public CatalogLocator {
  public:
    FileCatalogLocator(std::string id, std::string path)
        : id_(std::move(id)), path_(utils::path_from_utf8(std::move(path))) {}

    const std::string& id() const override { return id_; }

    std::optional<nlohmann::json> read(CatalogState* observed) const override {
        if (observed) {
            *observed = CatalogState{};
        }
        std::string body;
        CatalogState fresh;
        if (!read_body(path_, body, fresh)) {
            return std::nullopt;
        }
        nlohmann::json document = parse_document(id_, body);
        if (observed) {
            *observed = fresh;
        }
        return document;
    }

  private:
    std::string id_;
    fs::path path_;
};

std::string header_value(const utils::HttpResponse& response, const std::string& name) {
    const auto found = response.headers.find(name);
    return found == response.headers.end() ? std::string() : found->second;
}

std::string unique_temp_suffix() {
    static std::atomic<std::uint64_t> counter{0};
    return ".tmp." +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "." +
           std::to_string(counter.fetch_add(1));
}

void write_atomically(const fs::path& path, const std::string& text) {
    utils::ensure_create_directories(path.parent_path());
    const fs::path temp = path.string() + unique_temp_suffix();
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << text;
        out.flush();
        if (!out) {
            throw std::runtime_error("could not write " + utils::path_to_utf8(temp));
        }
    }
    std::error_code ec;
    fs::rename(temp, path, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(temp, ignored);
        throw std::runtime_error("could not replace " + utils::path_to_utf8(path));
    }
}

class HttpCatalogLocator final : public CatalogLocator {
  public:
    HttpCatalogLocator(std::string id, HttpCatalogSource source)
        : id_(std::move(id)), source_(std::move(source)) {}

    const std::string& id() const override { return id_; }

    std::optional<nlohmann::json> read(CatalogState* observed) const override {
        if (observed) {
            *observed = CatalogState{};
        }
        const CatalogState cached = cached_state();
        if (cached.present && served_from_this_process()) {
            if (auto document = serve_cache(observed, cached)) {
                return document;
            }
        }

        std::optional<utils::HttpResponse> response;
        try {
            response = utils::HttpClient::get(source_.url, conditional(cached), 0, policy(),
                                              source_.max_body_bytes);
        } catch (const std::exception& e) {
            LOG(WARNING, kLog) << "'" << id_ << "' did not answer: " << e.what() << std::endl;
        }

        if (response) {
            if (response->status_code == 304) {
                mark_fetched();
                return serve_cache(observed, cached);
            }
            if (response->status_code >= 200 && response->status_code < 300) {
                try {
                    nlohmann::json document = parse_document(id_, response->body);
                    // A body that does not parse never becomes the last good one.
                    write_cache(*response);
                    mark_fetched();
                    if (observed) {
                        *observed = state_of(*response);
                    }
                    return document;
                } catch (const std::exception& e) {
                    LOG(WARNING, kLog) << "'" << id_ << "' answered with " << e.what()
                                       << std::endl;
                }
            } else {
                LOG(WARNING, kLog) << "'" << id_ << "' answered " << response->status_code
                                   << std::endl;
            }
        }
        return serve_cache(observed, cached);
    }

  private:
    bool served_from_this_process() const {
        return source_.clock && source_.clock->fresh(id_, source_.fresh_for);
    }

    void mark_fetched() const {
        if (source_.clock) {
            source_.clock->mark_fetched(id_);
        }
    }

    fs::path body_path() const { return utils::path_from_utf8(source_.cache_path); }

    fs::path validators_path() const {
        return fs::path(body_path().string() + ".validators");
    }

    utils::HttpSecurityPolicy policy() const {
        // A stale opt-in cannot downgrade an https catalog.
        return source_.allow_insecure_http && source_.url.rfind("http://", 0) == 0
                   ? utils::HttpSecurityPolicy::AllowInsecureHttp
                   : utils::HttpSecurityPolicy::ExternalHttpsOnly;
    }

    std::map<std::string, std::string> conditional(const CatalogState& state) const {
        std::map<std::string, std::string> request;
        if (!state.etag.empty()) {
            request["If-None-Match"] = state.etag;
        }
        if (!state.last_modified.empty()) {
            request["If-Modified-Since"] = state.last_modified;
        }
        return request;
    }

    CatalogState cached_state() const {
        CatalogState state;
        std::error_code ec;
        const fs::path body = body_path();
        if (!fs::exists(body, ec)) {
            return state;
        }
        state.present = true;
        state.size = fs::file_size(body, ec);
        state.modified = fs::last_write_time(body, ec).time_since_epoch().count();

        std::ifstream in(validators_path(), std::ios::binary);
        if (in) {
            try {
                const nlohmann::json recorded = nlohmann::json::parse(in);
                state.etag = recorded.value("etag", std::string());
                state.last_modified = recorded.value("last_modified", std::string());
            } catch (const std::exception&) {
                // Nothing to revalidate with.
            }
        }
        return state;
    }

    CatalogState state_of(const utils::HttpResponse& response) const {
        CatalogState state;
        state.present = true;
        state.size = response.body.size();
        state.etag = header_value(response, "etag");
        state.last_modified = header_value(response, "last-modified");
        std::error_code ec;
        state.modified = fs::last_write_time(body_path(), ec).time_since_epoch().count();
        return state;
    }

    void write_cache(const utils::HttpResponse& response) const {
        nlohmann::json recorded;
        recorded["etag"] = header_value(response, "etag");
        recorded["last_modified"] = header_value(response, "last-modified");
        try {
            // Body first: an interrupted pair re-fetches, rather than answering 304
            // for a body that never landed.
            write_atomically(body_path(), response.body);
            write_atomically(validators_path(), recorded.dump());
        } catch (const std::exception& e) {
            LOG(WARNING, kLog) << "'" << id_ << "' could not cache its body: " << e.what()
                               << std::endl;
        }
    }

    std::optional<nlohmann::json> serve_cache(CatalogState* observed,
                                              const CatalogState& cached) const {
        std::string body;
        CatalogState served;
        if (!read_body(body_path(), body, served)) {
            return std::nullopt;
        }
        try {
            nlohmann::json document = parse_document(id_, body);
            if (observed) {
                served.etag = cached.etag;
                served.last_modified = cached.last_modified;
                *observed = served;
            }
            return document;
        } catch (const std::exception& e) {
            LOG(WARNING, kLog) << "'" << id_ << "' cached body is unusable: " << e.what()
                               << std::endl;
            return std::nullopt;
        }
    }

    std::string id_;
    HttpCatalogSource source_;
};

}  // namespace

std::shared_ptr<CatalogLocator> file_catalog_locator(std::string id, std::string path) {
    return std::make_shared<FileCatalogLocator>(std::move(id), std::move(path));
}

std::shared_ptr<CatalogLocator> http_catalog_locator(std::string id, HttpCatalogSource source) {
    return std::make_shared<HttpCatalogLocator>(std::move(id), std::move(source));
}

}  // namespace models
}  // namespace lemon

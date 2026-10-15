#include <lemon/models/catalog_locator.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;

namespace lemon {
namespace models {

namespace {

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
        : id_(std::move(id)), path_(std::move(path)) {}

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

    bool changed(const CatalogState& known) const override {
        std::error_code ec;
        if (!fs::exists(path_, ec)) {
            return known.present;
        }
        const std::int64_t modified = fs::last_write_time(path_, ec).time_since_epoch().count();
        const std::uint64_t size = fs::file_size(path_, ec);
        return !known.present || known.modified != modified || known.size != size;
    }

  private:
    std::string id_;
    fs::path path_;
};

}  // namespace

std::shared_ptr<CatalogLocator> file_catalog_locator(std::string id, std::string path) {
    return std::make_shared<FileCatalogLocator>(std::move(id), std::move(path));
}

}  // namespace models
}  // namespace lemon

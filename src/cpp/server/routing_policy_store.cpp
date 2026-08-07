#include "lemon/routing_policy_store.h"

#include "lemon/model_types.h"
#include "lemon/utils/aixlog.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace lemon {
namespace fs = std::filesystem;
namespace {

bool is_json_file(const fs::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".json";
}

json load_json_file(const fs::path& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("could not open file");
    }
    std::stringstream ss;
    ss << in.rdbuf();
    json parsed = json::parse(ss.str(), nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded()) {
        throw std::runtime_error("invalid JSON");
    }
    return parsed;
}

std::string policy_model_name(const json& doc, const fs::path& path) {
    if (doc.contains("model_name") && doc["model_name"].is_string() &&
        !doc["model_name"].get<std::string>().empty()) {
        return doc["model_name"].get<std::string>();
    }
    return path.stem().string();
}

bool is_router_policy_document(const json& doc) {
    return doc.is_object() && doc.contains("recipe") && doc["recipe"].is_string() &&
           is_router_collection_recipe(doc["recipe"].get<std::string>());
}

} // namespace

RoutingPolicyStore::RoutingPolicyStore(std::string directory,
                                       ClassifierServices services,
                                       RoutingPolicyParseOptions parse_options)
    : directory_(std::move(directory)),
      services_(std::move(services)),
      parse_options_(std::move(parse_options)),
      snapshot_(std::make_shared<Snapshot>()) {}

RoutingPolicyStore::~RoutingPolicyStore() {
    stop_watching();
}

std::shared_ptr<const RoutingPolicyStore::Snapshot> RoutingPolicyStore::load_directory() const {
    auto next = std::make_shared<Snapshot>();
    if (directory_.empty()) {
        return next;
    }

    std::error_code ec;
    if (!fs::exists(directory_, ec)) {
        return next;
    }

    std::map<std::string, std::string> policy_files_by_model;
    std::set<std::string> duplicate_models;

    for (fs::recursive_directory_iterator it(directory_, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (ec || !it->is_regular_file(ec) || !is_json_file(it->path())) {
            continue;
        }

        const std::string file = it->path().string();
        try {
            json doc = load_json_file(it->path());
            if (!is_router_policy_document(doc)) {
                continue;
            }

            const std::string model_name = policy_model_name(doc, it->path());
            auto [model_file_it, inserted] = policy_files_by_model.emplace(model_name, file);
            if (!inserted) {
                duplicate_models.insert(model_name);
                const std::string first_file = model_file_it->second;
                next->errors[file] = "duplicate collection.router model_name '" +
                                     model_name + "' also defined by '" +
                                     first_file + "'";
                if (next->errors.count(first_file) == 0) {
                    next->errors[first_file] =
                        "duplicate collection.router model_name '" + model_name +
                        "' also defined by '" + file + "'";
                }
                next->engines.erase(model_name);
                continue;
            }
            if (duplicate_models.count(model_name) != 0) {
                next->errors[file] =
                    "duplicate collection.router model_name '" + model_name + "'";
                continue;
            }

            RoutePolicy policy = parse_route_policy_collection(doc, parse_options_);
            auto engine = std::make_shared<RoutingPolicyEngine>(std::move(policy), services_);
            next->engines[model_name] = std::move(engine);
        } catch (const std::exception& e) {
            next->errors[file] = e.what();
        } catch (...) {
            next->errors[file] = "unknown routing policy load error";
        }
    }
    if (ec) {
        next->errors[directory_] = ec.message();
    }
    return next;
}

std::shared_ptr<const RoutingPolicyStore::Snapshot> RoutingPolicyStore::reload() {
    auto next = load_directory();
    std::atomic_store(&snapshot_, next);
    return next;
}

void RoutingPolicyStore::start_watching() {
    if (watcher_ || directory_.empty()) {
        return;
    }
    reload();
    watcher_ = std::make_unique<DirectoryWatcher>(directory_);
    watcher_->set_callback([this]() {
        try {
            auto next = reload();
            if (!next->errors.empty()) {
                LOG(WARNING, "Routing") << "Routing policy reload completed with "
                                        << next->errors.size() << " error(s)" << std::endl;
            }
        } catch (const std::exception& e) {
            LOG(WARNING, "Routing") << "Routing policy reload failed: "
                                    << e.what() << std::endl;
        } catch (...) {
            LOG(WARNING, "Routing") << "Routing policy reload failed with unknown error"
                                    << std::endl;
        }
    });
    watcher_->start();
}

void RoutingPolicyStore::stop_watching() {
    if (watcher_) {
        watcher_->stop();
        watcher_.reset();
    }
}

std::shared_ptr<const RoutingPolicyEngine> RoutingPolicyStore::get_engine(
    const std::string& model_name) const {
    auto current = snapshot();
    auto it = current->engines.find(model_name);
    return it == current->engines.end() ? nullptr : it->second;
}

std::shared_ptr<const RoutingPolicyStore::Snapshot> RoutingPolicyStore::snapshot() const {
    return std::atomic_load(&snapshot_);
}

} // namespace lemon

#include "lemon/server/route_registry.h"

#include <map>
#include <regex>
#include <stdexcept>

namespace lemon {

using json = nlohmann::json;

namespace {

const char* const kQuadPrefixes[] = {"/api/v0/", "/api/v1/", "/v0/", "/v1/"};

std::vector<std::string> prefixed_paths(const RouteSpec& spec, const std::string& path) {
    switch (spec.prefixes) {
        case Prefixes::Quad: {
            std::vector<std::string> urls;
            for (const char* prefix : kQuadPrefixes) {
                urls.push_back(prefix + path);
            }
            return urls;
        }
        case Prefixes::Internal:
            return {"/internal/" + path};
        case Prefixes::Root:
            return {path};
    }
    return {};
}

// httplib matches a route by regex, so each {name} in a path becomes a capture group.
// A Path arg's schema picks the group: its "pattern", digits for an integer, or one
// path segment otherwise.
std::string to_pattern(const RouteSpec& spec, const std::string& path) {
    std::string pattern;
    size_t pos = 0;
    while (true) {
        const size_t open = path.find('{', pos);
        if (open == std::string::npos) {
            pattern += path.substr(pos);
            return pattern;
        }
        const size_t close = path.find('}', open);
        if (close == std::string::npos) {
            throw std::logic_error(spec.id + ": unterminated parameter in path " + path);
        }
        pattern += path.substr(pos, open - pos);
        const std::string name = path.substr(open + 1, close - open - 1);
        const RouteArg* arg = nullptr;
        for (const auto& candidate : spec.args) {
            if (candidate.in == ArgIn::Path && candidate.name == name) {
                arg = &candidate;
            }
        }
        if (!arg) {
            throw std::logic_error(spec.id + ": path parameter {" + name +
                                   "} has no ArgIn::Path argument");
        }
        if (arg->schema.contains("pattern")) {
            pattern += "(" + arg->schema["pattern"].get<std::string>() + ")";
        } else if (arg->schema.value("type", std::string()) == "integer") {
            pattern += "(\\d+)";
        } else {
            pattern += "([^/]+)";
        }
        pos = close + 1;
    }
}

void method_not_allowed(const httplib::Request&, httplib::Response& res) {
    res.status = 405;
    res.set_content("{\"error\": \"Method Not Allowed. Use POST for this endpoint\"}",
                    "application/json");
}

} // namespace

void RouteRegistry::add(std::unique_ptr<ApiRoute> route) {
    auto entry = std::make_shared<Entry>();
    entry->spec = route->spec();
    const RouteSpec& spec = entry->spec;

    if (spec.id.empty() || spec.methods.empty() || spec.paths.empty()) {
        throw std::logic_error("Route '" + spec.id + "' needs an id, a method and a path");
    }
    for (const auto& existing : entries_) {
        if (existing->spec.id == spec.id) {
            throw std::logic_error("Route id '" + spec.id + "' is registered twice");
        }
    }
    if (dynamic_cast<ModelRoute*>(route.get()) && spec.request_format == RequestFormat::Raw) {
        // ModelRoute reads the model from the parsed body.
        throw std::logic_error("ModelRoute '" + spec.id + "' cannot use RequestFormat::Raw");
    }

    for (const auto& path : spec.paths) {
        const std::string pattern = to_pattern(spec, path);
        for (const auto& url : prefixed_paths(spec, pattern)) {
            entry->patterns.push_back(url);
        }
        if (spec.quiet_log) {
            for (const auto& url : prefixed_paths(spec, path)) {
                quiet_paths_.insert(url);
            }
        }
    }

    entry->route = std::move(route);
    entries_.push_back(std::move(entry));
}

void RouteRegistry::apply(httplib::Server& server) const {
    // A Quad path whose only method is POST also answers GET with 405, so a client that
    // forgot the method learns why instead of getting a 404.
    std::map<std::string, std::set<std::string>> methods_by_pattern;
    for (const auto& entry : entries_) {
        if (entry->spec.prefixes != Prefixes::Quad) continue;
        for (const auto& pattern : entry->patterns) {
            for (const auto& method : entry->spec.methods) {
                methods_by_pattern[pattern].insert(method);
            }
        }
    }

    for (const auto& entry : entries_) {
        std::shared_ptr<Entry> shared = entry;
        auto handler = [shared](const httplib::Request& req, httplib::Response& res) {
            shared->route->serve(shared->spec, req, res);
        };
        for (const auto& pattern : entry->patterns) {
            for (const auto& method : entry->spec.methods) {
                if (method == "GET") server.Get(pattern, handler);
                else if (method == "POST") server.Post(pattern, handler);
                else if (method == "DELETE") server.Delete(pattern, handler);
                else if (method == "PUT") server.Put(pattern, handler);
                else throw std::logic_error("Unsupported method " + method);
            }
            if (entry->spec.prefixes == Prefixes::Quad &&
                methods_by_pattern[pattern] == std::set<std::string>{"POST"}) {
                server.Get(pattern, method_not_allowed);
            }
        }
    }
}

json RouteRegistry::list() const {
    json routes = json::array();
    for (const auto& entry : entries_) {
        routes.push_back(route_spec_to_json(entry->spec));
    }
    return routes;
}

bool RouteRegistry::is_quiet(const std::string& path) const {
    return quiet_paths_.count(path) > 0;
}

bool RouteRegistry::dispatch(const httplib::Request& req, httplib::Response& res) const {
    for (const auto& entry : entries_) {
        bool method_matches = false;
        for (const auto& method : entry->spec.methods) {
            method_matches = method_matches || method == req.method;
        }
        if (!method_matches) continue;
        for (const auto& pattern : entry->patterns) {
            httplib::Request matched = req;
            if (std::regex_match(matched.path, matched.matches, std::regex(pattern))) {
                entry->route->serve(entry->spec, matched, res);
                return true;
            }
        }
    }
    return false;
}

} // namespace lemon

#include "lemon/server/web_ui.h"

#include <filesystem>
#include <fstream>
#include <string>

#include <lemon/utils/aixlog.hpp>
#include <nlohmann/json.hpp>

#include "lemon/model_manager.h"
#include "lemon/server/http_listener.h"
#include "lemon/server/model_json.h"
#include "lemon/utils/path_utils.h"

namespace fs = std::filesystem;

namespace lemon {

namespace {

void set_no_cache_headers(httplib::Response& res) {
    res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
    res.set_header("Pragma", "no-cache");
    res.set_header("Expires", "0");
}

bool read_file(const std::string& path, std::string& content) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    content.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

void replace_all(std::string& text, const std::string& from, const std::string& to) {
    size_t pos;
    while ((pos = text.find(from)) != std::string::npos) {
        text.replace(pos, from.size(), to);
    }
}

void serve_web_app_asset(const std::string& web_app_dir, const std::string& file_path,
                         httplib::Response& res) {
    std::error_code ec;

    auto base = fs::weakly_canonical(fs::path(web_app_dir), ec);
    if (ec) {
        res.status = 500;
        res.set_content("Internal server error", "text/plain");
        return;
    }

    auto candidate = fs::weakly_canonical(base / file_path, ec);
    if (ec) {
        res.status = 404;
        res.set_content("File not found", "text/plain");
        return;
    }

    // Verify the resolved path is confined under the base directory.
    // Use std::filesystem::relative (not string prefix) so it works
    // correctly on Windows where path separators differ.
    auto relative = fs::relative(candidate, base, ec);
    if (ec || relative.empty() || relative.is_absolute()) {
        // empty = candidate == base (directory, not a file)
        // is_absolute = somehow escaped (shouldn't happen after canonicalization)
        res.status = 403;
        res.set_content("Forbidden", "text/plain");
        return;
    }
    // Belt-and-suspenders: reject if any path component is ".."
    // (weakly_canonical should have resolved it, but this catches
    // edge cases on exotic filesystems). Check components, not
    // substrings, so legitimate filenames like "my..file.js" are allowed.
    for (const auto& part : relative) {
        if (part == "..") {
            res.status = 403;
            res.set_content("Forbidden", "text/plain");
            return;
        }
    }

    std::string content;
    if (!read_file(candidate.string(), content)) {
        res.status = 404;
        res.set_content("File not found", "text/plain");
        return;
    }

    std::string content_type = "application/octet-stream";
    size_t dot_pos = file_path.rfind('.');
    if (dot_pos != std::string::npos) {
        std::string ext = file_path.substr(dot_pos);
        if (ext == ".js") content_type = "text/javascript";
        else if (ext == ".css") content_type = "text/css";
        else if (ext == ".html") content_type = "text/html";
        else if (ext == ".woff") content_type = "font/woff";
        else if (ext == ".woff2") content_type = "font/woff2";
        else if (ext == ".ttf") content_type = "font/ttf";
        else if (ext == ".svg") content_type = "image/svg+xml";
        else if (ext == ".png") content_type = "image/png";
        else if (ext == ".jpg" || ext == ".jpeg") content_type = "image/jpeg";
        else if (ext == ".json") content_type = "application/json";
        else if (ext == ".ico") content_type = "image/x-icon";
    }

    res.set_content(content, content_type);
}

} // namespace

void WebUi::register_routes(httplib::Server& server) {
    std::string static_dir = utils::get_resource_path("resources/static");

    auto serve_status_page = [this, static_dir](const httplib::Request&, httplib::Response& res) {
        std::string index_path = static_dir + "/index.html";
        std::string html_template;
        if (!read_file(index_path, html_template)) {
            LOG(ERROR, "Server") << "Could not open index.html at: " << index_path << std::endl;
            res.status = 404;
            res.set_content("{\"error\": \"index.html not found\"}", "application/json");
            return;
        }

        nlohmann::json models = nlohmann::json::object();
        for (const auto& [model_name, info] : ctx_.model_manager->get_supported_models()) {
            models[model_name] = ctx_.model_json->status_page_json(model_name, info);
        }

        std::string platform_name;
        #ifdef _WIN32
            platform_name = "Windows";
        #elif __APPLE__
            platform_name = "Darwin";
        #elif __linux__
            platform_name = "Linux";
        #else
            platform_name = "Unknown";
        #endif

        replace_all(html_template, "{{SERVER_PORT}}", std::to_string(ctx_.listener->port()));
        replace_all(html_template, "{{SERVER_MODELS_JS}}",
                    "<script>window.SERVER_MODELS = " + models.dump() + ";</script>");
        replace_all(html_template, "{{PLATFORM_JS}}",
                    "<script>window.PLATFORM = '" + platform_name + "';</script>");

        set_no_cache_headers(res);
        res.set_content(html_template, "text/html");
    };

    server.Get("/api/v1", serve_status_page);

    if (!server.set_mount_point("/static", static_dir)) {
        LOG(WARNING, "Server") << "Could not mount static files from: " << static_dir << std::endl;
        LOG(WARNING, "Server") << "Status page assets will not be available" << std::endl;
    }

    std::string web_app_dir = utils::get_resource_path("resources/web-app");

    if (fs::exists(web_app_dir) && fs::is_directory(web_app_dir)) {
        auto serve_web_app_html = [web_app_dir](const httplib::Request&, httplib::Response& res) {
            std::string html;
            if (!read_file(web_app_dir + "/index.html", html)) {
                res.status = 404;
                res.set_content("{\"error\": \"Web app not found\"}", "application/json");
                return;
            }

            // Inject mock window.api for web compatibility with the shared Tauri app renderer
            std::string mock_api = R"(
<script>
// Mock window.api for web compatibility (the Tauri shim is skipped in pure-web mode)
window.api = {
    isWebApp: true,  // Explicit flag to indicate web mode
    platform: navigator.platform || 'web',
    minimizeWindow: () => {},
    maximizeWindow: () => {},
    closeWindow: () => {},
    openExternal: (url) => window.open(url, '_blank'),
    onMaximizeChange: () => {},
    updateMinWidth: () => {},
    zoomIn: () => document.body.style.zoom = (parseFloat(document.body.style.zoom || '1') + 0.1).toString(),
    zoomOut: () => document.body.style.zoom = (parseFloat(document.body.style.zoom || '1') - 0.1).toString(),
    getSettings: async () => {
        const saved = localStorage.getItem('lemonade-settings');
        if (saved) return JSON.parse(saved);
        // Return defaults matching DEFAULT_LAYOUT_SETTINGS from appSettings.ts
        return {
            layout: {
                isChatVisible: true,
                isModelManagerVisible: true,
                isCenterPanelVisible: true,
                isLogsVisible: false,
                modelManagerWidth: 280,
                chatWidth: 350,
                logsHeight: 200
            },
            theme: 'dark',
            apiUrl: window.location.origin,
            apiKey: { value: '' }
        };
    },
    saveSettings: async (settings) => {
        localStorage.setItem('lemonade-settings', JSON.stringify(settings));
        return settings;
    },
    onSettingsUpdated: () => {},
    getServerPort: () => parseInt(window.location.port) || 13305,
    onServerPortUpdated: () => {},
    getServerAPIKey: async () => {
        const settings = await window.api.getSettings();
        return settings.apiKey?.value || '';
    },
    restartApp: () => window.location.reload(),
    writeClipboard: async (text) => {
        if (navigator.clipboard) {
            try {
                await navigator.clipboard.writeText(text);
                return;
            } catch {
                // Ignore clipboard errors and fall back to legacy method
            }
        }
        const ta = document.createElement('textarea');
        ta.value = String(text);
        ta.setAttribute('readonly', '');
        ta.style.position = 'fixed';
        ta.style.left = '-9999px';
        document.body.appendChild(ta);
        try {
            ta.select();
            if (!document.execCommand('copy')) {
                throw new Error('Legacy clipboard copy failed');
            }
        } finally {
            document.body.removeChild(ta);
        }
    }
};
</script>
)";

            // Insert mock API before the closing </head> tag
            size_t head_end_pos = html.find("</head>");
            if (head_end_pos != std::string::npos) {
                html.insert(head_end_pos, mock_api);
            }

            set_no_cache_headers(res);
            res.set_content(html, "text/html");
        };

        server.Get("/", serve_web_app_html);

        // Also serve at /web-app for backwards compatibility
        server.Get("/web-app/?", serve_web_app_html);

        server.Get("/favicon.ico", [web_app_dir](const httplib::Request&, httplib::Response& res) {
            serve_web_app_asset(web_app_dir, "favicon.ico", res);
        });

        server.Get(R"(/([^/]+\.(js|css|woff|woff2|ttf|svg|png|jpg|jpeg|json|ico)))",
                   [web_app_dir](const httplib::Request& req, httplib::Response& res) {
            serve_web_app_asset(web_app_dir, req.matches[1].str(), res);
        });

        // Keep /web-app/ prefix routes for backwards compatibility
        server.Get(R"(/web-app/(.+))", [web_app_dir](const httplib::Request& req, httplib::Response& res) {
            serve_web_app_asset(web_app_dir, req.matches[1].str(), res);
        });

        // SPA fallback: serve index.html for any unmatched GET route outside the API,
        // so client-side routes survive a reload.
        server.Get(R"(^(?!/api|/v0|/v1|/static|/live|/status|/internal|/docs(/|$)).*)",
                   [serve_web_app_html](const httplib::Request& req, httplib::Response& res) {
            std::string path = req.path;
            size_t last_slash = path.rfind('/');
            std::string last_segment = (last_slash != std::string::npos) ? path.substr(last_slash + 1) : path;

            // A missing asset 404s instead of silently becoming the app shell.
            size_t dot_pos = last_segment.rfind('.');
            if (dot_pos != std::string::npos) {
                std::string ext = last_segment.substr(dot_pos);
                if (ext != ".html" && ext != ".htm") {
                    res.status = 404;
                    return;
                }
            }

            serve_web_app_html(req, res);
        });
    } else {
        LOG(INFO, "Server") << "Web app directory not found at: " << web_app_dir << std::endl;
        LOG(INFO, "Server") << "Falling back to static status page at root" << std::endl;

        server.Get("/", serve_status_page);

        server.Get("/favicon.ico", [static_dir](const httplib::Request&, httplib::Response& res) {
            std::string content;
            if (read_file(static_dir + "/favicon.ico", content)) {
                res.set_content(content, "image/x-icon");
                res.status = 200;
            } else {
                res.set_content("Favicon not found.", "text/plain");
                res.status = 404;
            }
        });
    }

    auto docs_not_found = [](const httplib::Request&, httplib::Response& res) {
        res.status = 404;
        res.set_content("{\"error\": \"Not Found. For API documentation, use /v1/docs.\"}", "application/json");
    };
    server.Get("/docs", docs_not_found);
    server.Get(R"(/docs/(.*))", docs_not_found);

    // Static files always get no-cache headers, so the web UI picks up a new version
    // as soon as lemond is updated.
    server.set_file_request_handler([](const httplib::Request&, httplib::Response& res) {
        set_no_cache_headers(res);
    });
}

} // namespace lemon

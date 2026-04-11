// Tauri app library entry — wires up plugins, commands, and background tasks.
// Mirrors the high-level responsibilities of the Electron main.js:
//   - Single-instance lock (second-instance focuses the existing window)
//   - Deep link handling for lemonade://
//   - UDP beacon listener for server port discovery
//   - macOS tray launcher on app start
//   - Maximize-change events forwarded to the renderer

pub mod beacon;
pub mod commands;
pub mod settings;
pub mod system_info;
pub mod tray_launcher;

use tauri::{Emitter, Manager, WindowEvent};

fn parse_protocol_url(url: &str) -> Option<serde_json::Value> {
    // lemonade://open?view=logs&model=foo
    if !url.starts_with("lemonade://") {
        return None;
    }
    // Use url::Url after replacing the scheme with http:// so the host/path
    // parse sensibly (url crate treats unknown schemes with no `//` as opaque).
    let stripped = url.trim_start_matches("lemonade://");
    // Accept either "open?..." or just "?..."
    let query_start = stripped.find('?').map(|i| i + 1).unwrap_or(stripped.len());
    let query = &stripped[query_start..];

    let mut out = serde_json::Map::new();
    for pair in query.split('&').filter(|p| !p.is_empty()) {
        let mut it = pair.splitn(2, '=');
        let key = it.next().unwrap_or("");
        let value = it.next().unwrap_or("");
        if key == "view" || key == "model" {
            out.insert(key.to_string(), serde_json::Value::String(value.to_string()));
        }
    }

    if out.is_empty() {
        None
    } else {
        Some(serde_json::Value::Object(out))
    }
}

fn handle_protocol_urls(app: &tauri::AppHandle, urls: &[String]) {
    for raw in urls {
        if let Some(nav) = parse_protocol_url(raw) {
            log::info!("Handling lemonade:// URL: {raw}");
            if let Some(window) = app.get_webview_window("main") {
                let _ = window.show();
                let _ = window.unminimize();
                let _ = window.set_focus();
            }
            // Queue for later if renderer isn't ready; otherwise emit immediately.
            if app.emit("navigate", nav.clone()).is_err() {
                commands::queue_pending_nav(nav);
            } else {
                commands::queue_pending_nav(nav);
            }
        }
    }
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info"))
        .format_timestamp_secs()
        .try_init()
        .ok();

    let mut builder = tauri::Builder::default();

    // Single instance (desktop only).
    #[cfg(desktop)]
    {
        builder = builder.plugin(tauri_plugin_single_instance::init(|app, args, _cwd| {
            log::info!("Second instance launched with args: {:?}", args);
            if let Some(window) = app.get_webview_window("main") {
                let _ = window.show();
                let _ = window.unminimize();
                let _ = window.set_focus();
            }
            // Find any lemonade:// URL in the CLI args (Windows ships them that way).
            let urls: Vec<String> = args
                .iter()
                .filter(|s| s.starts_with("lemonade://"))
                .cloned()
                .collect();
            if !urls.is_empty() {
                handle_protocol_urls(app, &urls);
            }
        }));
    }

    builder
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_clipboard_manager::init())
        .plugin(tauri_plugin_deep_link::init())
        .setup(|app| {
            let app_handle = app.handle().clone();

            // Start macOS tray if needed (no-op elsewhere)
            tray_launcher::ensure_tray_running();

            // Background beacon listener — async task on Tauri's runtime
            let listener_handle = app_handle.clone();
            tauri::async_runtime::spawn(async move {
                beacon::run_beacon_listener(listener_handle).await;
            });

            // Register deep-link handler (macOS open-url, Linux xdg-open, etc.)
            #[cfg(desktop)]
            {
                use tauri_plugin_deep_link::DeepLinkExt;
                let deep_link_handle = app_handle.clone();
                app.deep_link().on_open_url(move |event| {
                    let urls: Vec<String> =
                        event.urls().iter().map(|u| u.to_string()).collect();
                    handle_protocol_urls(&deep_link_handle, &urls);
                });

                // Also register `lemonade` scheme at runtime (no-op if the OS already
                // knows via the installer/Info.plist). Dev builds need this.
                let _ = app.deep_link().register("lemonade");
            }

            // Forward maximize/unmaximize as "maximize-change" events so
            // TitleBar.tsx stays in sync with the window state.
            if let Some(window) = app.get_webview_window("main") {
                let emitter = app_handle.clone();
                let window_clone = window.clone();
                window.on_window_event(move |event| {
                    if matches!(event, WindowEvent::Resized(_)) {
                        if let Ok(maximized) = window_clone.is_maximized() {
                            let _ = emitter.emit("maximize-change", maximized);
                        }
                    }
                });
            }

            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            commands::minimize_window,
            commands::maximize_window,
            commands::close_window,
            commands::update_min_width,
            commands::zoom_in,
            commands::zoom_out,
            commands::get_app_settings,
            commands::save_app_settings,
            commands::get_version,
            commands::get_system_stats,
            commands::get_system_info,
            commands::get_server_base_url,
            commands::get_server_api_key,
            commands::get_server_port,
            commands::discover_server_port,
            commands::get_platform,
            commands::get_local_marketplace_url,
            commands::renderer_ready,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_protocol_url_extracts_view_and_model() {
        let nav = parse_protocol_url("lemonade://open?view=logs&model=foo").unwrap();
        assert_eq!(nav.get("view").unwrap(), "logs");
        assert_eq!(nav.get("model").unwrap(), "foo");
    }

    #[test]
    fn parse_protocol_url_returns_none_for_empty_query() {
        assert!(parse_protocol_url("lemonade://open").is_none());
    }

    #[test]
    fn parse_protocol_url_rejects_other_schemes() {
        assert!(parse_protocol_url("http://example.com").is_none());
    }
}

// Tauri host: window, single-instance lock, deep-link handler, UDP beacon
// listener, macOS tray bootstrap, and the invoke command surface consumed by
// `src/app/src/renderer/tauriShim.ts`.

pub mod beacon;
pub mod commands;
pub mod events;
pub mod settings;
pub mod tray_launcher;
pub mod webview_shim;

use tauri::{Emitter, Manager, WindowEvent};

fn parse_protocol_url(raw: &str) -> Option<serde_json::Value> {
    // lemonade://open?view=logs&model=foo
    // The url crate treats unknown schemes as opaque, so swap to http:// for
    // parsing and rely on query_pairs() instead of hand-splitting on '?'/'&'.
    let normalized = raw.strip_prefix("lemonade://")?;
    let parsed = url::Url::parse(&format!("http://lemonade/{normalized}")).ok()?;
    let mut out = serde_json::Map::new();
    for (key, value) in parsed.query_pairs() {
        if key == "view" || key == "model" {
            out.insert(
                key.into_owned(),
                serde_json::Value::String(value.into_owned()),
            );
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
            // If the renderer hasn't mounted yet, stash the nav for
            // `renderer_ready` to drain. Otherwise fire-and-forget.
            if commands::is_renderer_ready() {
                let _ = app.emit(events::NAVIGATE, nav);
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

            // Forward maximize state changes so TitleBar.tsx stays in sync
            // with the window. Emitted on every resize, guarded by
            // change-detection to avoid flooding the renderer.
            if let Some(window) = app.get_webview_window("main") {
                webview_shim::apply(&window);

                let emitter = app_handle.clone();
                let window_clone = window.clone();
                let last_maximized = std::sync::atomic::AtomicBool::new(false);
                window.on_window_event(move |event| {
                    if matches!(event, WindowEvent::Resized(_)) {
                        if let Ok(maximized) = window_clone.is_maximized() {
                            let prev = last_maximized.swap(
                                maximized,
                                std::sync::atomic::Ordering::Relaxed,
                            );
                            if prev != maximized {
                                let _ = emitter.emit(events::MAXIMIZE_CHANGE, maximized);
                            }
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

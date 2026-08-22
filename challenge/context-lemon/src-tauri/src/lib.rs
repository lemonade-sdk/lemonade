mod commands;
mod config;
pub mod indexing;
pub mod jobs;
pub mod lemonade;
pub mod rag;
pub mod watcher;

use config::ConfigState;
use indexing::store::VectorStore;
use jobs::IndexJobs;
use lemonade::LemonadeClient;
use std::sync::Mutex;
use tauri::menu::{Menu, MenuItem, PredefinedMenuItem};
use tauri::tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent};
use tauri::{Emitter, Manager, WindowEvent};

/// Passed by the autostart registration so a login launch can go straight to the tray.
const AUTOSTART_FLAG: &str = "--autostart";

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    let config = config::load();
    let lemonade_url = config.lemonade_url.clone();

    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_dialog::init())
        // The registered autostart entry passes `--autostart`, which is how we tell an
        // OS-triggered launch from a user double-click. Without it the app would pop a
        // window at every login that nobody asked for.
        .plugin(tauri_plugin_autostart::init(
            tauri_plugin_autostart::MacosLauncher::LaunchAgent,
            Some(vec![AUTOSTART_FLAG]),
        ))
        .manage(ConfigState(Mutex::new(config)))
        .manage(VectorStore::open(config::index_path()))
        .manage(LemonadeClient::new(lemonade_url))
        .manage(IndexJobs::default())
        .invoke_handler(tauri::generate_handler![
            commands::get_config,
            commands::get_index_status,
            commands::take_index_load_error,
            commands::remove_folder,
            commands::add_folder_dialog,
            commands::index_folder_command,
            commands::ask_question,
            commands::check_lemonade,
        ])
        .setup(|app| {
            let show_i = MenuItem::with_id(app, "show", "Open", true, None::<&str>)?;
            let add_folder_i =
                MenuItem::with_id(app, "add_folder", "Add Folder…", true, None::<&str>)?;
            let quit_i = MenuItem::with_id(app, "quit", "Quit", true, None::<&str>)?;
            let separator = PredefinedMenuItem::separator(app)?;
            let menu = Menu::with_items(app, &[&show_i, &add_folder_i, &separator, &quit_i])?;

            TrayIconBuilder::new()
                .icon(app.default_window_icon().unwrap().clone())
                .menu(&menu)
                .show_menu_on_left_click(false)
                .on_menu_event(|app, event| match event.id.as_ref() {
                    "show" => {
                        if let Some(window) = app.get_webview_window("main") {
                            let _ = window.show();
                            let _ = window.set_focus();
                        }
                    }
                    "add_folder" => {
                        commands::add_folder_dialog(app.clone());
                    }
                    "quit" => {
                        app.exit(0);
                    }
                    _ => {}
                })
                .on_tray_icon_event(|tray, event| {
                    if let TrayIconEvent::Click {
                        button: MouseButton::Left,
                        button_state: MouseButtonState::Up,
                        ..
                    } = event
                    {
                        let app = tray.app_handle();
                        if let Some(window) = app.get_webview_window("main") {
                            let _ = window.show();
                            let _ = window.set_focus();
                        }
                    }
                })
                .build(app)?;

            // Launched by the OS at login: stay in the tray rather than stealing focus
            // with a window. A user-initiated launch still opens normally.
            if std::env::args().any(|arg| arg == AUTOSTART_FLAG) {
                if let Some(window) = app.get_webview_window("main") {
                    let _ = window.hide();
                }
            }

            seed_and_backfill_index(app.handle());
            watcher::start(app.handle().clone());

            Ok(())
        })
        .on_window_event(|window, event| {
            if let WindowEvent::CloseRequested { api, .. } = event {
                // Closing the window hides it to tray instead of quitting the app.
                let _ = window.hide();
                api.prevent_close();
                let _ = window.emit("window-hidden", ());
            }
        })
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}

/// Resolve `..` segments so the path shown in the UI (and stored in config) is the
/// real location. `canonicalize` does that but returns a `\\?\` verbatim path on
/// Windows, which is correct yet unreadable, so strip that prefix for display.
fn tidy_path(path: std::path::PathBuf) -> std::path::PathBuf {
    let Ok(canonical) = path.canonicalize() else {
        return path;
    };
    let text = canonical.to_string_lossy().to_string();
    match text.strip_prefix(r"\\?\") {
        // Leave UNC verbatim paths alone — stripping would turn \\?\UNC\srv\share
        // into the invalid UNC\srv\share.
        Some(rest) if !rest.starts_with("UNC\\") => std::path::PathBuf::from(rest),
        _ => canonical,
    }
}

fn locate_sample_folder(app: &tauri::AppHandle) -> Option<std::path::PathBuf> {
    if let Ok(resource_dir) = app.path().resource_dir() {
        // Tauri rewrites the `..` in the `"../sample"` resource entry to `_up_`, so the
        // bundled copy lands at <resources>/_up_/sample, not <resources>/sample.
        for candidate in [resource_dir.join("_up_").join("sample"), resource_dir.join("sample")] {
            if candidate.exists() {
                return Some(tidy_path(candidate));
            }
        }
    }
    // Dev-only fallback: an absolute compile-time path that exists on the build
    // machine. Never present in a packaged install.
    let dev_candidate = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../sample");
    if dev_candidate.exists() {
        return Some(tidy_path(dev_candidate));
    }
    None
}

/// Shared indexing job so the tray, the frontend command and startup all report
/// progress the same way.
pub(crate) async fn run_index_job(app: &tauri::AppHandle, folder: &str) {
    let store = app.state::<VectorStore>();
    let client = app.state::<LemonadeClient>();
    let jobs = app.state::<IndexJobs>();

    let token = jobs.start(folder);
    let _ = app.emit("index-progress", folder);
    let progress_app = app.clone();
    let on_progress = |p: rag::IndexProgress| {
        let _ = progress_app.emit("index-progress-detail", p);
    };
    let result = rag::index_folder(&store, &client, folder, &token, &on_progress).await;
    jobs.finish(folder, &token);

    match result {
        Ok(stats) => {
            let _ = app.emit("index-updated", store.stats());
            let _ = app.emit("index-done", stats);
        }
        Err(err) => {
            let _ = app.emit("index-error", err);
        }
    }
}

/// Register the bundled sample folder on first run, and index any configured folder
/// that has no chunks yet. The second half matters: if the very first index failed
/// (e.g. Lemonade wasn't running), the folder is already saved to config, so gating
/// purely on "config is empty" would leave it registered but never indexed with no
/// way back short of deleting config.json.
fn seed_and_backfill_index(app: &tauri::AppHandle) {
    let config_state = app.state::<ConfigState>();

    let is_empty = config_state.0.lock().unwrap().folders.is_empty();
    if is_empty {
        if let Some(sample_dir) = locate_sample_folder(app) {
            let sample_str = sample_dir.to_string_lossy().to_string();
            let mut cfg = config_state.0.lock().unwrap();
            cfg.folders.push(sample_str);
            let _ = config::save(&cfg);
        }
    }

    let folders = config_state.0.lock().unwrap().folders.clone();
    if folders.is_empty() {
        return;
    }

    let (_, chunks) = app.state::<VectorStore>().stats();
    let index_is_empty = chunks == 0;

    let app_handle = app.clone();
    tauri::async_runtime::spawn(async move {
        // Only backfill when there is nothing indexed at all; a populated index is
        // refreshed by the watcher / explicit re-index rather than on every launch.
        if !index_is_empty {
            return;
        }
        for folder in folders {
            run_index_job(&app_handle, &folder).await;
        }
    });
}

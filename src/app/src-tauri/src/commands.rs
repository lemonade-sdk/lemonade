//! Tauri invoke handlers backing the renderer's `window.api` surface.

use crate::beacon;
use crate::events;
use crate::settings::{self, AppSettings};
use crate::tray_launcher;
use serde::Serialize;
use serde_json::Value;
use tauri::{AppHandle, Emitter, Manager, WebviewWindow};

// ---------- Window controls ----------

fn main_window(app: &AppHandle) -> Option<WebviewWindow> {
    app.get_webview_window("main")
}

#[tauri::command]
pub(crate) fn minimize_window(app: AppHandle) {
    if let Some(w) = main_window(&app) {
        let _ = w.minimize();
    }
}

#[tauri::command]
pub(crate) fn maximize_window(app: AppHandle) {
    if let Some(w) = main_window(&app) {
        if let Ok(true) = w.is_maximized() {
            let _ = w.unmaximize();
        } else {
            let _ = w.maximize();
        }
    }
}

#[tauri::command]
pub(crate) fn close_window(app: AppHandle) {
    if let Some(w) = main_window(&app) {
        let _ = w.close();
    }
}

const ABSOLUTE_MIN_WIDTH: f64 = 400.0;
const DEFAULT_MIN_HEIGHT: f64 = 600.0;

#[tauri::command]
pub(crate) fn update_min_width(app: AppHandle, width: f64) {
    if !width.is_finite() {
        return;
    }
    let safe_width = width.round().max(ABSOLUTE_MIN_WIDTH);
    if let Some(w) = main_window(&app) {
        let _ = w.set_min_size(Some(tauri::LogicalSize::new(
            safe_width,
            DEFAULT_MIN_HEIGHT,
        )));
    }
}

// Zoom level state is stored on the webview itself via get/set_zoom.
// We use discrete steps to mirror Electron's main.js behavior.
const MIN_ZOOM_FACTOR: f64 = 0.5;
const MAX_ZOOM_FACTOR: f64 = 2.5;
const ZOOM_STEP: f64 = 0.1;

fn clamp_zoom(factor: f64) -> f64 {
    factor.max(MIN_ZOOM_FACTOR).min(MAX_ZOOM_FACTOR)
}

// We track zoom factor in a mutex because Tauri's webview.set_zoom doesn't
// expose a getter on all platforms.
static CURRENT_ZOOM: std::sync::Mutex<f64> = std::sync::Mutex::new(1.0);

#[tauri::command]
pub(crate) fn zoom_in(app: AppHandle) {
    let mut current = CURRENT_ZOOM.lock().unwrap();
    *current = clamp_zoom(*current + ZOOM_STEP);
    if let Some(w) = main_window(&app) {
        let _ = w.set_zoom(*current);
    }
}

#[tauri::command]
pub(crate) fn zoom_out(app: AppHandle) {
    let mut current = CURRENT_ZOOM.lock().unwrap();
    *current = clamp_zoom(*current - ZOOM_STEP);
    if let Some(w) = main_window(&app) {
        let _ = w.set_zoom(*current);
    }
}

// ---------- Settings ----------

#[derive(Debug, Clone, Serialize)]
pub(crate) struct ConnectionSettings {
    pub base_url: String,
    pub api_key: String,
}

#[tauri::command]
pub(crate) fn get_app_settings() -> AppSettings {
    settings::read_app_settings()
}

#[tauri::command]
pub(crate) fn save_app_settings(app: AppHandle, payload: Value) -> Result<AppSettings, String> {
    let sanitized = settings::write_app_settings(&payload)?;
    let _ = app.emit(events::SETTINGS_UPDATED, &sanitized);
    let _ = app.emit(
        events::CONNECTION_SETTINGS_UPDATED,
        ConnectionSettings {
            base_url: sanitized
                .base_url
                .value
                .as_str()
                .unwrap_or_default()
                .to_string(),
            api_key: sanitized
                .api_key
                .value
                .as_str()
                .unwrap_or_default()
                .to_string(),
        },
    );
    Ok(sanitized)
}

// ---------- Server info ----------
// /health, /system-stats, and /system-info are fetched directly from the
// renderer via `serverConfig.fetch(...)` — no Rust proxy needed. The host
// only owns the connection-settings half (base URL, API key, port) so the
// renderer knows where to send those requests.

#[tauri::command]
pub(crate) fn get_server_base_url() -> Option<String> {
    settings::get_base_url_from_config()
}

#[tauri::command]
pub(crate) fn get_server_api_key() -> String {
    settings::get_api_key_from_config()
}

#[tauri::command]
pub(crate) fn get_server_port() -> u16 {
    beacon::get_cached_port()
}

#[tauri::command]
pub(crate) async fn discover_server_port(app: AppHandle) -> Option<u16> {
    if settings::get_base_url_from_config().is_some() {
        log::info!("Port discovery skipped - explicit server URL configured");
        tray_launcher::ensure_tray_running();
        return None;
    }

    let port = beacon::discover_server_port_once().await;
    beacon::set_cached_port(port);
    let _ = app.emit(events::SERVER_PORT_UPDATED, port);
    Some(port)
}

// ---------- Misc ----------

#[tauri::command]
pub(crate) fn get_platform() -> String {
    std::env::consts::OS.to_string()
}

// Returns a file:// URL for the bundled marketplace.html if it exists.
// In dev mode, falls back to <project>/docs/marketplace.html via the resource dir.
#[tauri::command]
pub(crate) fn get_local_marketplace_url(app: AppHandle) -> Option<String> {
    // Try bundled resource first
    if let Ok(resource) = app.path().resource_dir() {
        let candidate = resource.join("docs").join("marketplace.html");
        if candidate.exists() {
            return Some(format!(
                "file://{}?embedded=true&theme=dark",
                candidate.to_string_lossy()
            ));
        }
    }
    None
}

// ---------- Renderer ready + deep-link queue ----------

// Pending deep-link navigation for when the renderer hasn't mounted yet.
// `RENDERER_READY` flips to true on the first `renderer_ready` command and
// stays true for the rest of the process lifetime. Use it to decide whether
// to emit a nav immediately or park it in `PENDING_NAV` for later drain.
static PENDING_NAV: std::sync::Mutex<Option<Value>> = std::sync::Mutex::new(None);
static RENDERER_READY: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

pub(crate) fn is_renderer_ready() -> bool {
    RENDERER_READY.load(std::sync::atomic::Ordering::Acquire)
}

pub(crate) fn queue_pending_nav(data: Value) {
    *PENDING_NAV.lock().unwrap() = Some(data);
}

fn take_pending_nav() -> Option<Value> {
    PENDING_NAV.lock().unwrap().take()
}

#[tauri::command]
pub(crate) fn renderer_ready(app: AppHandle) {
    RENDERER_READY.store(true, std::sync::atomic::Ordering::Release);
    if let Some(data) = take_pending_nav() {
        let _ = app.emit(crate::events::NAVIGATE, data);
    }
}

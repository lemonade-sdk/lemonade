// #[tauri::command] functions that back the renderer's `window.api` surface.
// Each command maps 1:1 to an IPC method the Electron main.js used to provide
// via `ipcMain.handle` / `ipcMain.on`.

use crate::beacon;
use crate::settings::{self, AppSettings};
use crate::system_info::{self, SystemInfo, SystemStats};
use crate::tray_launcher;
use serde::Serialize;
use serde_json::Value;
use tauri::{AppHandle, Emitter, Manager, WebviewWindow};

// ---------- Window controls ----------

fn main_window(app: &AppHandle) -> Option<WebviewWindow> {
    app.get_webview_window("main")
}

#[tauri::command]
pub fn minimize_window(app: AppHandle) {
    if let Some(w) = main_window(&app) {
        let _ = w.minimize();
    }
}

#[tauri::command]
pub fn maximize_window(app: AppHandle) {
    if let Some(w) = main_window(&app) {
        if let Ok(true) = w.is_maximized() {
            let _ = w.unmaximize();
        } else {
            let _ = w.maximize();
        }
    }
}

#[tauri::command]
pub fn close_window(app: AppHandle) {
    if let Some(w) = main_window(&app) {
        let _ = w.close();
    }
}

const ABSOLUTE_MIN_WIDTH: f64 = 400.0;
const DEFAULT_MIN_HEIGHT: f64 = 600.0;

#[tauri::command]
pub fn update_min_width(app: AppHandle, width: f64) {
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
pub fn zoom_in(app: AppHandle) {
    let mut current = CURRENT_ZOOM.lock().unwrap();
    *current = clamp_zoom(*current + ZOOM_STEP);
    if let Some(w) = main_window(&app) {
        let _ = w.set_zoom(*current);
    }
}

#[tauri::command]
pub fn zoom_out(app: AppHandle) {
    let mut current = CURRENT_ZOOM.lock().unwrap();
    *current = clamp_zoom(*current - ZOOM_STEP);
    if let Some(w) = main_window(&app) {
        let _ = w.set_zoom(*current);
    }
}

// ---------- Settings ----------

pub const SETTINGS_UPDATED_EVENT: &str = "settings-updated";
pub const CONNECTION_SETTINGS_UPDATED_EVENT: &str = "connection-settings-updated";

#[derive(Debug, Clone, Serialize)]
pub struct ConnectionSettings {
    pub base_url: String,
    pub api_key: String,
}

#[tauri::command]
pub fn get_app_settings() -> AppSettings {
    settings::read_app_settings()
}

#[tauri::command]
pub fn save_app_settings(app: AppHandle, payload: Value) -> Result<AppSettings, String> {
    let sanitized = settings::write_app_settings(&payload)?;
    let _ = app.emit(SETTINGS_UPDATED_EVENT, &sanitized);
    let _ = app.emit(
        CONNECTION_SETTINGS_UPDATED_EVENT,
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

#[tauri::command]
pub async fn get_version() -> String {
    system_info::fetch_version().await
}

#[tauri::command]
pub async fn get_system_stats() -> SystemStats {
    system_info::fetch_system_stats().await
}

#[tauri::command]
pub async fn get_system_info() -> SystemInfo {
    system_info::fetch_system_info().await
}

#[tauri::command]
pub fn get_server_base_url() -> Option<String> {
    settings::get_base_url_from_config()
}

#[tauri::command]
pub fn get_server_api_key() -> String {
    settings::get_api_key_from_config()
}

#[tauri::command]
pub fn get_server_port() -> u16 {
    beacon::get_cached_port()
}

#[tauri::command]
pub async fn discover_server_port(app: AppHandle) -> Option<u16> {
    if settings::get_base_url_from_config().is_some() {
        log::info!("Port discovery skipped - explicit server URL configured");
        tray_launcher::ensure_tray_running();
        return None;
    }

    let port = beacon::discover_server_port_once().await;
    beacon::set_cached_port(port);
    let _ = app.emit(beacon::SERVER_PORT_UPDATED_EVENT, port);
    Some(port)
}

// ---------- Misc ----------

#[tauri::command]
pub fn get_platform() -> String {
    std::env::consts::OS.to_string()
}

// Returns a file:// URL for the bundled marketplace.html if it exists.
// In dev mode, falls back to <project>/docs/marketplace.html via the resource dir.
#[tauri::command]
pub fn get_local_marketplace_url(app: AppHandle) -> Option<String> {
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

// Pending deep link navigation delivered before the renderer mounted.
// Drained on the `renderer_ready` command.
static PENDING_NAV: std::sync::Mutex<Option<Value>> = std::sync::Mutex::new(None);

pub fn queue_pending_nav(data: Value) {
    let mut slot = PENDING_NAV.lock().unwrap();
    *slot = Some(data);
}

pub fn take_pending_nav() -> Option<Value> {
    PENDING_NAV.lock().unwrap().take()
}

#[tauri::command]
pub fn renderer_ready(app: AppHandle) {
    if let Some(data) = take_pending_nav() {
        let _ = app.emit("navigate", data);
    }
}

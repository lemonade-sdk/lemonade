//! Tauri event channel names. One source of truth shared between the Rust host
//! and the TypeScript renderer shim (which hard-codes matching strings in
//! `src/app/src/renderer/tauriShim.ts` — keep those in sync when editing here).

pub(crate) const SETTINGS_UPDATED: &str = "settings-updated";
pub(crate) const CONNECTION_SETTINGS_UPDATED: &str = "connection-settings-updated";
pub(crate) const SERVER_PORT_UPDATED: &str = "server-port-updated";
pub(crate) const REMOTE_DEVICES_UPDATED: &str = "remote-devices-updated";
pub(crate) const MAXIMIZE_CHANGE: &str = "maximize-change";
pub(crate) const NAVIGATE: &str = "navigate";

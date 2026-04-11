// Port of Electron main.js settings management (lines 27-349).
// Reads/writes app_settings.json from the Lemonade cache directory (~/.cache/lemonade/),
// sanitizing values and applying defaults for missing/invalid fields.

use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::fs;
use std::path::PathBuf;

const SETTINGS_FILE_NAME: &str = "app_settings.json";

// ---------- Default values (match main.js) ----------

fn default_temperature() -> f64 {
    0.7
}
fn default_top_k() -> i64 {
    40
}
fn default_top_p() -> f64 {
    0.9
}
fn default_repeat_penalty() -> f64 {
    1.1
}
fn default_enable_thinking() -> bool {
    true
}
fn default_collapse_thinking() -> bool {
    false
}
fn default_base_url() -> String {
    String::new()
}
fn default_api_key() -> String {
    String::new()
}

fn default_layout() -> LayoutSettings {
    LayoutSettings {
        is_chat_visible: true,
        is_model_manager_visible: true,
        is_marketplace_visible: true,
        is_logs_visible: false,
        model_manager_width: 280,
        chat_width: 350,
        logs_height: 200,
    }
}

fn default_tts() -> TtsSettings {
    TtsSettings {
        model: TypedSetting {
            value: Value::String("kokoro-v1".to_string()),
            use_default: true,
        },
        user_voice: TypedSetting {
            value: Value::String("fable".to_string()),
            use_default: true,
        },
        assistant_voice: TypedSetting {
            value: Value::String("alloy".to_string()),
            use_default: true,
        },
        enable_tts: TypedSetting {
            value: Value::Bool(false),
            use_default: true,
        },
        enable_user_tts: TypedSetting {
            value: Value::Bool(false),
            use_default: true,
        },
    }
}

// ---------- Types ----------

// Generic { value, useDefault } setting — value type varies, so Value.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TypedSetting {
    pub value: Value,
    #[serde(rename = "useDefault")]
    pub use_default: bool,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct LayoutSettings {
    pub is_chat_visible: bool,
    pub is_model_manager_visible: bool,
    pub is_marketplace_visible: bool,
    pub is_logs_visible: bool,
    pub model_manager_width: i64,
    pub chat_width: i64,
    pub logs_height: i64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct TtsSettings {
    pub model: TypedSetting,
    pub user_voice: TypedSetting,
    pub assistant_voice: TypedSetting,
    pub enable_tts: TypedSetting,
    pub enable_user_tts: TypedSetting,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct AppSettings {
    pub temperature: TypedSetting,
    pub top_k: TypedSetting,
    pub top_p: TypedSetting,
    pub repeat_penalty: TypedSetting,
    pub enable_thinking: TypedSetting,
    pub collapse_thinking_by_default: TypedSetting,
    pub base_url: TypedSetting,
    pub api_key: TypedSetting,
    pub layout: LayoutSettings,
    pub tts: TtsSettings,
}

impl Default for AppSettings {
    fn default() -> Self {
        AppSettings {
            temperature: TypedSetting {
                value: json_num(default_temperature()),
                use_default: true,
            },
            top_k: TypedSetting {
                value: Value::from(default_top_k()),
                use_default: true,
            },
            top_p: TypedSetting {
                value: json_num(default_top_p()),
                use_default: true,
            },
            repeat_penalty: TypedSetting {
                value: json_num(default_repeat_penalty()),
                use_default: true,
            },
            enable_thinking: TypedSetting {
                value: Value::Bool(default_enable_thinking()),
                use_default: true,
            },
            collapse_thinking_by_default: TypedSetting {
                value: Value::Bool(default_collapse_thinking()),
                use_default: true,
            },
            base_url: TypedSetting {
                value: Value::String(default_base_url()),
                use_default: true,
            },
            api_key: TypedSetting {
                value: Value::String(default_api_key()),
                use_default: true,
            },
            layout: default_layout(),
            tts: default_tts(),
        }
    }
}

// ---------- Path helpers ----------

fn cache_directory() -> Option<PathBuf> {
    let home = dirs::home_dir()?;
    Some(home.join(".cache").join("lemonade"))
}

fn settings_file_path() -> Option<PathBuf> {
    Some(cache_directory()?.join(SETTINGS_FILE_NAME))
}

// ---------- Clamp / sanitize helpers ----------

fn json_num(value: f64) -> Value {
    serde_json::Number::from_f64(value)
        .map(Value::Number)
        .unwrap_or(Value::Null)
}

fn clamp_f64(value: f64, min: f64, max: f64) -> f64 {
    if !value.is_finite() {
        return min;
    }
    value.max(min).min(max)
}

fn clamp_i64(value: i64, min: i64, max: i64) -> i64 {
    value.max(min).min(max)
}

// Sanitize an incoming JSON blob into an AppSettings.
// Mirrors main.js `sanitizeAppSettings` (lines 196-317). For each field:
//   - if the field is present and well-formed, use its value clamped to limits
//   - otherwise fall back to the default
// Numeric `useDefault` means "ignore the provided value, keep the default".
pub fn sanitize_app_settings(incoming: &Value) -> AppSettings {
    let mut sanitized = AppSettings::default();

    // Numeric settings: temperature, topK, topP, repeatPenalty
    let numeric_limits: &[(&str, f64, f64, bool)] = &[
        ("temperature", 0.0, 2.0, false),
        ("topK", 1.0, 100.0, true),
        ("topP", 0.0, 1.0, false),
        ("repeatPenalty", 1.0, 2.0, false),
    ];

    for (key, min, max, is_int) in numeric_limits.iter().copied() {
        if let Some(raw) = incoming.get(key).and_then(Value::as_object) {
            let use_default = raw.get("useDefault").and_then(Value::as_bool).unwrap_or(true);
            let current_slot = match key {
                "temperature" => &mut sanitized.temperature,
                "topK" => &mut sanitized.top_k,
                "topP" => &mut sanitized.top_p,
                "repeatPenalty" => &mut sanitized.repeat_penalty,
                _ => unreachable!(),
            };
            current_slot.use_default = use_default;
            if !use_default {
                if let Some(raw_val) = raw.get("value").and_then(Value::as_f64) {
                    let clamped = clamp_f64(raw_val, min, max);
                    current_slot.value = if is_int {
                        Value::from(clamped as i64)
                    } else {
                        json_num(clamped)
                    };
                }
            }
        }
    }

    // enableThinking
    if let Some(raw) = incoming.get("enableThinking").and_then(Value::as_object) {
        let use_default = raw
            .get("useDefault")
            .and_then(Value::as_bool)
            .unwrap_or(sanitized.enable_thinking.use_default);
        sanitized.enable_thinking.use_default = use_default;
        if !use_default {
            if let Some(v) = raw.get("value").and_then(Value::as_bool) {
                sanitized.enable_thinking.value = Value::Bool(v);
            }
        }
    }

    // collapseThinkingByDefault
    if let Some(raw) = incoming.get("collapseThinkingByDefault").and_then(Value::as_object) {
        let use_default = raw
            .get("useDefault")
            .and_then(Value::as_bool)
            .unwrap_or(sanitized.collapse_thinking_by_default.use_default);
        sanitized.collapse_thinking_by_default.use_default = use_default;
        if !use_default {
            if let Some(v) = raw.get("value").and_then(Value::as_bool) {
                sanitized.collapse_thinking_by_default.value = Value::Bool(v);
            }
        }
    }

    // baseURL
    if let Some(raw) = incoming.get("baseURL").and_then(Value::as_object) {
        let use_default = raw
            .get("useDefault")
            .and_then(Value::as_bool)
            .unwrap_or(sanitized.base_url.use_default);
        sanitized.base_url.use_default = use_default;
        if !use_default {
            if let Some(v) = raw.get("value").and_then(Value::as_str) {
                sanitized.base_url.value = Value::String(v.to_string());
            }
        }
    }

    // apiKey
    if let Some(raw) = incoming.get("apiKey").and_then(Value::as_object) {
        let use_default = raw
            .get("useDefault")
            .and_then(Value::as_bool)
            .unwrap_or(sanitized.api_key.use_default);
        sanitized.api_key.use_default = use_default;
        if !use_default {
            if let Some(v) = raw.get("value").and_then(Value::as_str) {
                sanitized.api_key.value = Value::String(v.to_string());
            }
        }
    }

    // layout
    if let Some(raw_layout) = incoming.get("layout").and_then(Value::as_object) {
        // Booleans
        for (key, slot) in [
            (
                "isChatVisible",
                &mut sanitized.layout.is_chat_visible as *mut bool,
            ),
            (
                "isModelManagerVisible",
                &mut sanitized.layout.is_model_manager_visible as *mut bool,
            ),
            (
                "isMarketplaceVisible",
                &mut sanitized.layout.is_marketplace_visible as *mut bool,
            ),
            (
                "isLogsVisible",
                &mut sanitized.layout.is_logs_visible as *mut bool,
            ),
        ] {
            if let Some(v) = raw_layout.get(key).and_then(Value::as_bool) {
                // SAFETY: each pointer refers to a distinct field of `sanitized.layout`
                // and we only use it inside this block.
                unsafe {
                    *slot = v;
                }
            }
        }

        // Numeric layout sizes with limits
        let layout_limits: &[(&str, i64, i64)] = &[
            ("modelManagerWidth", 200, 500),
            ("chatWidth", 250, 800),
            ("logsHeight", 100, 400),
        ];
        for (key, min, max) in layout_limits.iter().copied() {
            if let Some(v) = raw_layout.get(key).and_then(Value::as_f64) {
                if v.is_finite() {
                    let rounded = v.round() as i64;
                    let clamped = clamp_i64(rounded, min, max);
                    match key {
                        "modelManagerWidth" => sanitized.layout.model_manager_width = clamped,
                        "chatWidth" => sanitized.layout.chat_width = clamped,
                        "logsHeight" => sanitized.layout.logs_height = clamped,
                        _ => {}
                    }
                }
            }
        }
    }

    // tts: sanitize each field similarly
    if let Some(raw_tts) = incoming.get("tts").and_then(Value::as_object) {
        for (key, slot) in [
            ("model", &mut sanitized.tts.model),
            ("userVoice", &mut sanitized.tts.user_voice),
            ("assistantVoice", &mut sanitized.tts.assistant_voice),
            ("enableTTS", &mut sanitized.tts.enable_tts),
            ("enableUserTTS", &mut sanitized.tts.enable_user_tts),
        ] {
            if let Some(raw) = raw_tts.get(key).and_then(Value::as_object) {
                let use_default = raw
                    .get("useDefault")
                    .and_then(Value::as_bool)
                    .unwrap_or(slot.use_default);
                slot.use_default = use_default;
                if !use_default {
                    if let Some(v) = raw.get("value") {
                        // Only string or bool accepted (mirrors main.js)
                        if v.is_string() || v.is_boolean() {
                            slot.value = v.clone();
                        }
                    }
                }
            }
        }
    }

    sanitized
}

// ---------- Read / write ----------

pub fn read_app_settings() -> AppSettings {
    let Some(path) = settings_file_path() else {
        return AppSettings::default();
    };

    match fs::read_to_string(&path) {
        Ok(content) => match serde_json::from_str::<Value>(&content) {
            Ok(value) => sanitize_app_settings(&value),
            Err(err) => {
                log::error!("Failed to parse app settings file: {err}");
                AppSettings::default()
            }
        },
        Err(err) => {
            if err.kind() != std::io::ErrorKind::NotFound {
                log::error!("Failed to read app settings file: {err}");
            }
            AppSettings::default()
        }
    }
}

pub fn write_app_settings(incoming: &Value) -> Result<AppSettings, String> {
    let path = settings_file_path()
        .ok_or_else(|| "Unable to locate the Lemonade home directory".to_string())?;

    let sanitized = sanitize_app_settings(incoming);

    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|e| format!("create_dir_all failed: {e}"))?;
    }

    let json = serde_json::to_string_pretty(&sanitized)
        .map_err(|e| format!("serialize failed: {e}"))?;
    fs::write(&path, json).map_err(|e| format!("write failed: {e}"))?;

    Ok(sanitized)
}

// Extract just the base URL string from settings (returns None if unset or invalid).
// Mirrors main.js `getBaseURLFromConfig` + `normalizeServerUrl` (lines 52-78, 355-367).
pub fn get_base_url_from_config() -> Option<String> {
    let settings = read_app_settings();
    let raw = settings.base_url.value.as_str()?;
    normalize_server_url(raw)
}

pub fn get_api_key_from_config() -> String {
    let settings = read_app_settings();
    settings
        .api_key
        .value
        .as_str()
        .map(ToString::to_string)
        .unwrap_or_default()
}

pub fn normalize_server_url(url: &str) -> Option<String> {
    let trimmed = url.trim();
    if trimmed.is_empty() {
        return None;
    }

    let with_scheme = if trimmed.to_ascii_lowercase().starts_with("http://")
        || trimmed.to_ascii_lowercase().starts_with("https://")
    {
        trimmed.to_string()
    } else {
        format!("http://{trimmed}")
    };

    let trimmed_trailing = with_scheme.trim_end_matches('/').to_string();

    match url::Url::parse(&trimmed_trailing) {
        Ok(_) => Some(trimmed_trailing),
        Err(err) => {
            log::warn!("Invalid server URL {url}: {err}");
            None
        }
    }
}

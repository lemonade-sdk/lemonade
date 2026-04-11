//! App-settings persistence. Reads and writes `~/.cache/lemonade/app_settings.json`,
//! sanitizing values and applying defaults for missing/invalid fields before
//! handing the struct back to the renderer. The JSON shape matches what the
//! existing React renderer expects (each user-tunable field is a
//! `{ value, useDefault }` envelope).

use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::fs;
use std::path::PathBuf;

const SETTINGS_FILE_NAME: &str = "app_settings.json";

// ---------- Default values ----------

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
                value: Value::Bool(true),
                use_default: true,
            },
            collapse_thinking_by_default: TypedSetting {
                value: Value::Bool(false),
                use_default: true,
            },
            base_url: TypedSetting {
                value: Value::String(String::new()),
                use_default: true,
            },
            api_key: TypedSetting {
                value: Value::String(String::new()),
                use_default: true,
            },
            layout: default_layout(),
            tts: default_tts(),
        }
    }
}

// ---------- Path helpers ----------

fn settings_file_path() -> Option<PathBuf> {
    Some(dirs::home_dir()?.join(".cache").join("lemonade").join(SETTINGS_FILE_NAME))
}

// ---------- Sanitize helpers ----------

fn json_num(value: f64) -> Value {
    serde_json::Number::from_f64(value)
        .map(Value::Number)
        .unwrap_or(Value::Null)
}

/// Apply a typed setting from `incoming[key]` onto `slot`, filtering the new
/// value through `extract`. If `useDefault` is true or the raw value fails
/// extraction, the slot's existing default is preserved.
fn apply_typed_setting<F>(incoming: &Value, key: &str, slot: &mut TypedSetting, extract: F)
where
    F: FnOnce(&Value) -> Option<Value>,
{
    let Some(raw) = incoming.get(key).and_then(Value::as_object) else {
        return;
    };
    if let Some(use_default) = raw.get("useDefault").and_then(Value::as_bool) {
        slot.use_default = use_default;
    }
    if slot.use_default {
        return;
    }
    if let Some(new_value) = raw.get("value").and_then(extract) {
        slot.value = new_value;
    }
}

fn extract_bool(v: &Value) -> Option<Value> {
    v.as_bool().map(Value::Bool)
}

fn extract_string(v: &Value) -> Option<Value> {
    v.as_str().map(|s| Value::String(s.to_string()))
}

fn extract_clamped_f64(min: f64, max: f64) -> impl Fn(&Value) -> Option<Value> {
    move |v| v.as_f64().map(|n| json_num(n.clamp(min, max)))
}

fn extract_clamped_i64(min: i64, max: i64) -> impl Fn(&Value) -> Option<Value> {
    move |v| {
        v.as_f64()
            .filter(|n| n.is_finite())
            .map(|n| Value::from(((n.round()) as i64).clamp(min, max)))
    }
}

/// Sanitize an incoming JSON blob into an `AppSettings`. For each field, use
/// the provided value (clamped to its valid range) when `useDefault` is false;
/// otherwise keep the default. Mirrors the original Electron main.js behavior
/// byte-for-byte so renderer state round-trips without drift.
pub(crate) fn sanitize_app_settings(incoming: &Value) -> AppSettings {
    let mut s = AppSettings::default();

    apply_typed_setting(incoming, "temperature", &mut s.temperature, extract_clamped_f64(0.0, 2.0));
    apply_typed_setting(incoming, "topK", &mut s.top_k, extract_clamped_i64(1, 100));
    apply_typed_setting(incoming, "topP", &mut s.top_p, extract_clamped_f64(0.0, 1.0));
    apply_typed_setting(
        incoming,
        "repeatPenalty",
        &mut s.repeat_penalty,
        extract_clamped_f64(1.0, 2.0),
    );
    apply_typed_setting(incoming, "enableThinking", &mut s.enable_thinking, extract_bool);
    apply_typed_setting(
        incoming,
        "collapseThinkingByDefault",
        &mut s.collapse_thinking_by_default,
        extract_bool,
    );
    apply_typed_setting(incoming, "baseURL", &mut s.base_url, extract_string);
    apply_typed_setting(incoming, "apiKey", &mut s.api_key, extract_string);

    if let Some(raw_layout) = incoming.get("layout").and_then(Value::as_object) {
        let set_bool = |key: &str, slot: &mut bool| {
            if let Some(v) = raw_layout.get(key).and_then(Value::as_bool) {
                *slot = v;
            }
        };
        set_bool("isChatVisible", &mut s.layout.is_chat_visible);
        set_bool("isModelManagerVisible", &mut s.layout.is_model_manager_visible);
        set_bool("isMarketplaceVisible", &mut s.layout.is_marketplace_visible);
        set_bool("isLogsVisible", &mut s.layout.is_logs_visible);

        let clamp_size = |key: &str, min: i64, max: i64, slot: &mut i64| {
            if let Some(v) = raw_layout.get(key).and_then(Value::as_f64) {
                if v.is_finite() {
                    *slot = (v.round() as i64).clamp(min, max);
                }
            }
        };
        clamp_size("modelManagerWidth", 200, 500, &mut s.layout.model_manager_width);
        clamp_size("chatWidth", 250, 800, &mut s.layout.chat_width);
        clamp_size("logsHeight", 100, 400, &mut s.layout.logs_height);
    }

    if let Some(raw_tts) = incoming.get("tts").and_then(Value::as_object) {
        let raw_tts_value = Value::Object(raw_tts.clone());
        for (key, slot) in [
            ("model", &mut s.tts.model),
            ("userVoice", &mut s.tts.user_voice),
            ("assistantVoice", &mut s.tts.assistant_voice),
            ("enableTTS", &mut s.tts.enable_tts),
            ("enableUserTTS", &mut s.tts.enable_user_tts),
        ] {
            apply_typed_setting(&raw_tts_value, key, slot, |v| {
                if v.is_string() || v.is_boolean() {
                    Some(v.clone())
                } else {
                    None
                }
            });
        }
    }

    s
}

// ---------- Read / write ----------

pub(crate) fn read_app_settings() -> AppSettings {
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
        Err(err) if err.kind() == std::io::ErrorKind::NotFound => AppSettings::default(),
        Err(err) => {
            log::error!("Failed to read app settings file: {err}");
            AppSettings::default()
        }
    }
}

pub(crate) fn write_app_settings(incoming: &Value) -> Result<AppSettings, String> {
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

pub(crate) fn get_base_url_from_config() -> Option<String> {
    normalize_server_url(read_app_settings().base_url.value.as_str()?)
}

pub(crate) fn get_api_key_from_config() -> String {
    read_app_settings()
        .api_key
        .value
        .as_str()
        .map(ToString::to_string)
        .unwrap_or_default()
}

pub(crate) fn normalize_server_url(url: &str) -> Option<String> {
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

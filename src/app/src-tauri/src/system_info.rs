//! HTTP helpers that proxy `/api/v1/health`, `/system-stats`, and `/system-info`
//! from the running `lemond` server to the renderer, normalizing the payloads
//! into the shape the React UI expects.

use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::sync::OnceLock;

use crate::beacon;
use crate::settings;

fn base_url() -> String {
    settings::get_base_url_from_config()
        .unwrap_or_else(|| format!("http://localhost:{}", beacon::get_cached_port()))
}

/// Shared reqwest client — keeps the connection pool alive across the many
/// polling calls the status bar makes. `reqwest::Client` is cheap to clone
/// internally so a single instance is the right pattern here.
fn http_client() -> &'static reqwest::Client {
    static CLIENT: OnceLock<reqwest::Client> = OnceLock::new();
    CLIENT.get_or_init(|| {
        reqwest::Client::builder()
            .timeout(std::time::Duration::from_millis(3000))
            .build()
            .unwrap_or_else(|_| reqwest::Client::new())
    })
}

async fn fetch_with_api_key(endpoint: &str) -> Result<Value, String> {
    let url = format!("{}{}", base_url(), endpoint);
    let api_key = settings::get_api_key_from_config();
    let mut req = http_client().get(&url);
    if !api_key.is_empty() {
        req = req.header("Authorization", format!("Bearer {api_key}"));
    }
    let res = req.send().await.map_err(|e| e.to_string())?;
    if !res.status().is_success() {
        return Err(format!("HTTP {}", res.status()));
    }
    res.json::<Value>().await.map_err(|e| e.to_string())
}

// ---- Version ----

pub(crate) async fn fetch_version() -> String {
    match fetch_with_api_key("/api/v1/health").await {
        Ok(v) => v
            .get("version")
            .and_then(Value::as_str)
            .unwrap_or("Unknown")
            .to_string(),
        Err(err) => {
            log::warn!("Failed to fetch version from server: {err}");
            "Unknown".to_string()
        }
    }
}

// ---- System stats ----

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub struct SystemStats {
    pub cpu_percent: Option<f64>,
    pub memory_gb: f64,
    pub gpu_percent: Option<f64>,
    pub vram_gb: Option<f64>,
    pub npu_percent: Option<f64>,
}

pub(crate) async fn fetch_system_stats() -> SystemStats {
    match fetch_with_api_key("/api/v1/system-stats").await {
        Ok(data) => SystemStats {
            cpu_percent: data.get("cpu_percent").and_then(Value::as_f64),
            memory_gb: data
                .get("memory_gb")
                .and_then(Value::as_f64)
                .unwrap_or(0.0),
            gpu_percent: data.get("gpu_percent").and_then(Value::as_f64),
            vram_gb: data.get("vram_gb").and_then(Value::as_f64),
            npu_percent: data.get("npu_percent").and_then(Value::as_f64),
        },
        Err(err) => {
            log::warn!("Failed to fetch system stats: {err}");
            SystemStats::default()
        }
    }
}

// ---- System info ----

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SystemInfo {
    pub system: String,
    pub os: String,
    pub cpu: String,
    pub gpus: Vec<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub gtt_gb: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub vram_gb: Option<String>,
}

impl Default for SystemInfo {
    fn default() -> Self {
        SystemInfo {
            system: "Unknown".to_string(),
            os: "Unknown".to_string(),
            cpu: "Unknown".to_string(),
            gpus: Vec::new(),
            gtt_gb: Some("Unknown".to_string()),
            vram_gb: Some("Unknown".to_string()),
        }
    }
}

pub(crate) async fn fetch_system_info() -> SystemInfo {
    let data = match fetch_with_api_key("/api/v1/system-info").await {
        Ok(d) => d,
        Err(err) => {
            log::warn!("Failed to fetch system info: {err}");
            return SystemInfo::default();
        }
    };

    let mut info = SystemInfo {
        system: "Unknown".to_string(),
        os: data
            .get("OS Version")
            .and_then(Value::as_str)
            .unwrap_or("Unknown")
            .to_string(),
        cpu: data
            .get("Processor")
            .and_then(Value::as_str)
            .unwrap_or("Unknown")
            .to_string(),
        gpus: Vec::new(),
        gtt_gb: None,
        vram_gb: None,
    };

    let mut max_gtt_gb = 0.0_f64;
    let mut max_vram_gb = 0.0_f64;

    let mut consider_amd = |gpu: &Value| {
        if let Some(v) = gpu.get("virtual_mem_gb").and_then(Value::as_f64) {
            if v.is_finite() && v > max_gtt_gb {
                max_gtt_gb = v;
            }
        }
        if let Some(v) = gpu.get("vram_gb").and_then(Value::as_f64) {
            if v.is_finite() && v > max_vram_gb {
                max_vram_gb = v;
            }
        }
    };

    if let Some(devices) = data.get("devices") {
        if let Some(amd_igpu) = devices.get("amd_igpu") {
            consider_amd(amd_igpu);
            if let Some(name) = amd_igpu.get("name").and_then(Value::as_str) {
                info.gpus.push(name.to_string());
            }
        }
        if let Some(amd_dgpu_list) = devices.get("amd_dgpu").and_then(Value::as_array) {
            for gpu in amd_dgpu_list {
                consider_amd(gpu);
                if let Some(name) = gpu.get("name").and_then(Value::as_str) {
                    info.gpus.push(name.to_string());
                }
            }
        }
        if let Some(nvidia_igpu) = devices.get("nvidia_igpu") {
            if let Some(name) = nvidia_igpu.get("name").and_then(Value::as_str) {
                info.gpus.push(name.to_string());
            }
        }
        if let Some(nvidia_dgpu_list) = devices.get("nvidia_dgpu").and_then(Value::as_array) {
            for gpu in nvidia_dgpu_list {
                if let Some(name) = gpu.get("name").and_then(Value::as_str) {
                    info.gpus.push(name.to_string());
                }
            }
        }
    }

    if max_gtt_gb > 0.0 {
        info.gtt_gb = Some(format!("{} GB", max_gtt_gb));
    }
    if max_vram_gb > 0.0 {
        info.vram_gb = Some(format!("{} GB", max_vram_gb));
    }

    info
}

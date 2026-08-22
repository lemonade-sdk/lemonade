use serde::{Deserialize, Serialize};
use std::fs;
use std::path::PathBuf;
use std::sync::Mutex;

#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct Config {
    pub folders: Vec<String>,
    #[serde(default = "default_lemonade_url")]
    pub lemonade_url: String,
}

fn default_lemonade_url() -> String {
    "http://localhost:13305/v1".to_string()
}

impl Default for Config {
    fn default() -> Self {
        Config {
            folders: Vec::new(),
            lemonade_url: default_lemonade_url(),
        }
    }
}

pub struct ConfigState(pub Mutex<Config>);

pub fn config_dir() -> PathBuf {
    let base = dirs::config_dir().unwrap_or_else(std::env::temp_dir);
    base.join("lemonade-context-engine")
}

fn config_path() -> PathBuf {
    config_dir().join("config.json")
}

pub fn index_path() -> PathBuf {
    config_dir().join("index.bin")
}

pub fn load() -> Config {
    let path = config_path();
    match fs::read_to_string(&path) {
        Ok(raw) => serde_json::from_str(&raw).unwrap_or_default(),
        Err(_) => Config::default(),
    }
}

pub fn save(config: &Config) -> std::io::Result<()> {
    let dir = config_dir();
    fs::create_dir_all(&dir)?;
    let raw = serde_json::to_string_pretty(config)?;
    fs::write(config_path(), raw)
}

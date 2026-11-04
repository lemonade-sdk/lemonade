fn main() {
    println!("cargo:rerun-if-env-changed=LEMONADE_TAURI_VERSION");
    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows") {
        if let Ok(version) = std::env::var("LEMONADE_TAURI_VERSION") {
            std::env::set_var("CARGO_PKG_VERSION", version);
        }
    }
    tauri_build::build()
}

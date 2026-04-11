// Port of Electron main.js `ensureTrayRunning` + `gracefulKillBlocking`
// (main.js lines 406-494). macOS-only; spawns `/usr/local/bin/lemonade-server tray`
// as a detached process if the tray isn't already running. On other platforms
// this is a no-op.

#[cfg(target_os = "macos")]
pub fn ensure_tray_running() {
    use std::path::Path;
    use std::process::{Command, Stdio};
    use std::thread;
    use std::time::Duration;

    const BINARY_PATH: &str = "/usr/local/bin/lemonade-server";
    const LOCK_FILE: &str = "/tmp/lemonade_Tray.lock";
    const KILL_TIMEOUT_SECS: u64 = 30;

    if !Path::new(BINARY_PATH).exists() {
        log::error!("CRITICAL: Binary not found at {BINARY_PATH}");
        return;
    }

    log::info!("--- STARTING TRAY MANUALLY ---");

    // Nuclear cleanup: kill stale tray processes + remove lock file
    let kill_status = Command::new("pkill")
        .args(["-f", "lemonade-server tray"])
        .status();

    if let Ok(status) = kill_status {
        if status.success() {
            // Poll for exit
            let deadline = std::time::Instant::now() + Duration::from_secs(KILL_TIMEOUT_SECS);
            loop {
                if std::time::Instant::now() >= deadline {
                    // Force kill
                    let _ = Command::new("pkill")
                        .args(["-9", "-f", "lemonade-server tray"])
                        .status();
                    break;
                }
                let check = Command::new("pgrep")
                    .args(["-f", "lemonade-server tray"])
                    .status();
                if let Ok(s) = check {
                    if !s.success() {
                        break;
                    }
                }
                thread::sleep(Duration::from_secs(1));
            }
        }
    }

    // Remove the stale lock file
    if Path::new(LOCK_FILE).exists() {
        let _ = std::fs::remove_file(LOCK_FILE);
    }

    // Prepare environment: macOS GUI apps don't have /usr/local/bin in PATH.
    let mut path = std::env::var("PATH").unwrap_or_default();
    if !path.contains("/usr/local/bin") {
        path.push_str(":/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin");
    }
    let mut dyld = std::env::var("DYLD_LIBRARY_PATH").unwrap_or_default();
    if !dyld.contains("/usr/local/lib") {
        if !dyld.is_empty() {
            dyld.push(':');
        }
        dyld.push_str("/usr/local/lib");
    }

    // Launch tray detached
    log::info!("Spawning tray process...");
    let spawn_result = Command::new(BINARY_PATH)
        .arg("tray")
        .env("PATH", path)
        .env("DYLD_LIBRARY_PATH", dyld)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn();

    match spawn_result {
        Ok(child) => log::info!("Tray launched (PID: {})", child.id()),
        Err(err) => log::error!("Failed to spawn tray: {err}"),
    }

    // Give it a moment to initialize
    thread::sleep(Duration::from_secs(1));
}

#[cfg(not(target_os = "macos"))]
pub fn ensure_tray_running() {
    // Windows + Linux: tray lifecycle is managed elsewhere
    // (Windows startup folder / Linux autostart) — nothing to do from the app.
}

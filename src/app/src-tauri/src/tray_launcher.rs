//! macOS-only helper that ensures the `lemonade-server tray` process is
//! running when the desktop app launches. On Windows and Linux the tray is
//! started by system autostart so there's nothing to do.

#[cfg(target_os = "macos")]
mod imp {
    use std::path::Path;
    use std::process::{Command, Stdio};
    use std::thread;
    use std::time::{Duration, Instant};

    const BINARY_PATH: &str = "/usr/local/bin/lemonade-server";
    const LOCK_FILE: &str = "/tmp/lemonade_Tray.lock";
    const KILL_TIMEOUT_SECS: u64 = 30;

    fn graceful_kill_tray() {
        // SIGTERM first; if anything is still running after KILL_TIMEOUT_SECS
        // fall back to SIGKILL. `pkill` returns non-zero when no process
        // matches, which we treat as "already clean".
        match Command::new("pkill").args(["-f", "lemonade-server tray"]).status() {
            Ok(status) if status.success() => {}
            _ => return,
        }

        let deadline = Instant::now() + Duration::from_secs(KILL_TIMEOUT_SECS);
        while Instant::now() < deadline {
            let still_alive = Command::new("pgrep")
                .args(["-f", "lemonade-server tray"])
                .status()
                .map(|s| s.success())
                .unwrap_or(false);
            if !still_alive {
                return;
            }
            thread::sleep(Duration::from_secs(1));
        }
        let _ = Command::new("pkill")
            .args(["-9", "-f", "lemonade-server tray"])
            .status();
    }

    pub(super) fn spawn() {
        if !Path::new(BINARY_PATH).exists() {
            log::error!("CRITICAL: Binary not found at {BINARY_PATH}");
            return;
        }

        log::info!("--- STARTING TRAY MANUALLY ---");
        graceful_kill_tray();

        if Path::new(LOCK_FILE).exists() {
            let _ = std::fs::remove_file(LOCK_FILE);
        }

        // macOS GUI apps don't inherit /usr/local/bin in PATH, and runtime
        // libraries live in /usr/local/lib — restore both so the tray can
        // find its tools and DYLDs.
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

        log::info!("Spawning tray process...");
        match Command::new(BINARY_PATH)
            .arg("tray")
            .env("PATH", path)
            .env("DYLD_LIBRARY_PATH", dyld)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
        {
            Ok(child) => log::info!("Tray launched (PID: {})", child.id()),
            Err(err) => log::error!("Failed to spawn tray: {err}"),
        }
    }
}

/// Fire-and-forget the tray bootstrap on a dedicated OS thread. The worst-case
/// kill-and-wait loop can block for up to 30 seconds, which would freeze the
/// Tauri main thread if we called it synchronously from `setup()`.
pub(crate) fn ensure_tray_running() {
    #[cfg(target_os = "macos")]
    {
        std::thread::spawn(imp::spawn);
    }
    #[cfg(not(target_os = "macos"))]
    {
        // no-op on Windows/Linux
    }
}

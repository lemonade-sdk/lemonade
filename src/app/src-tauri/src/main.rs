// Prevents additional console window on Windows in release, DO NOT REMOVE!!
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

// WebKit2GTK 2.42+ enables a DMA-BUF renderer that creates an EGL display via
// EGL_PLATFORM_GBM. Inside the AppImage the bundled libwayland-egl mixes with
// the host's libEGL/libgbm and the call fails with EGL_BAD_PARAMETER, killing
// the WebKit GPU subprocess and leaving a black window. The AppImage runtime
// sets $APPIMAGE, so we gate on that to avoid touching dev/deb/rpm builds.
#[cfg(target_os = "linux")]
fn apply_appimage_workarounds() {
    if std::env::var_os("APPIMAGE").is_none() {
        return;
    }
    if std::env::var_os("WEBKIT_DISABLE_DMABUF_RENDERER").is_none() {
        std::env::set_var("WEBKIT_DISABLE_DMABUF_RENDERER", "1");
    }
}

fn main() {
    #[cfg(target_os = "linux")]
    apply_appimage_workarounds();
    lemonade_app_lib::run()
}

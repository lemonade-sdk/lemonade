//! Platform-specific webview tweaks the original Electron app got "for free"
//! from Chromium that don't have a tauri.conf.json equivalent:
//!
//! 1. **External-link interception** — anchor clicks targeting `http(s)://`
//!    URLs (including inside cross-origin iframes like the marketplace) should
//!    open in the system browser instead of navigating the webview. The
//!    Electron version did this via `setWindowOpenHandler` + per-frame
//!    `executeJavaScript`. The Tauri equivalent: register a *user script*
//!    via the platform-native webview API so it runs in every frame. The
//!    script intercepts clicks; the top frame calls `window.api.openExternal`
//!    directly, child frames postMessage their hrefs up to the top frame,
//!    which routes them through `openExternal`.
//!
//! 2. **Microphone permission auto-grant** — the original transcription
//!    panel relied on Electron's `setPermissionRequestHandler` granting
//!    `media` without prompting. Per platform:
//!      - Linux (webkit2gtk): not auto-granted by wry; we connect the
//!        `permission-request` signal and allow `UserMediaPermissionRequest`,
//!        plus enable `media-stream` / `webrtc` / `mediasource` settings.
//!      - macOS (WKWebView): wry's default `WryWebViewUIDelegate` already
//!        auto-grants every media-capture request — nothing to do here.
//!      - Windows (WebView2): not auto-granted by wry (only clipboard is);
//!        we subscribe to `PermissionRequested` and allow Microphone and
//!        Camera.
//!
//! 3. **Linux-only navigation policy hook** — for any non-click navigation
//!    to an external URL (programmatic `location.href = ...`, an iframe
//!    setting its src to an http(s) link, etc.), webkit2gtk's `decide-policy`
//!    signal is intercepted and the request is routed to `openExternal`
//!    instead. macOS/Windows have analogous hooks but they would require
//!    replacing wry's existing navigation delegate; the user-script approach
//!    covers the click case which is by far the most common.

use tauri::WebviewWindow;

const CLICK_INTERCEPTOR_JS: &str = r#"
(() => {
  if (window.__lemonadeExternalLinkInterceptorInstalled) return;
  window.__lemonadeExternalLinkInterceptorInstalled = true;

  // Intercept anchor clicks in this frame. http(s) URLs are routed to the
  // top frame for delegation to window.api.openExternal.
  document.addEventListener('click', (event) => {
    const el = event.target && event.target.closest
      ? event.target.closest('a[href]')
      : null;
    if (!el) return;
    const href = el.href || el.getAttribute('href') || '';
    if (!/^https?:\/\//i.test(href)) return;
    event.preventDefault();
    event.stopPropagation();
    if (window.parent !== window) {
      // Cross/same-origin iframe -> postMessage the href up to the top frame.
      try {
        window.parent.postMessage({ __lemonadeOpenExternal: true, href }, '*');
      } catch (_) {
        // Cross-origin postMessage occasionally throws on detached frames; ignore.
      }
    } else if (window.api && typeof window.api.openExternal === 'function') {
      window.api.openExternal(href);
    } else {
      // tauriShim.ts hasn't installed window.api yet (very early click).
      // Fall back to window.open which Tauri routes through wry's default
      // new-window handler.
      window.open(href, '_blank', 'noopener,noreferrer');
    }
  }, true);

  // Top frame: receive forwarded hrefs from child frames.
  if (window.parent === window) {
    window.addEventListener('message', (event) => {
      const data = event && event.data;
      if (data && data.__lemonadeOpenExternal && typeof data.href === 'string'
          && /^https?:\/\//i.test(data.href)) {
        if (window.api && typeof window.api.openExternal === 'function') {
          window.api.openExternal(data.href);
        }
      }
    });
  }
})();
"#;

/// Install every platform-specific shim. Called once from `setup()` after the
/// main window has been created.
pub(crate) fn apply(window: &WebviewWindow) {
    if let Err(err) = window.with_webview(install_platform_shim) {
        log::warn!("Failed to install platform webview shim: {err}");
    }
}

// ---------- Linux (webkit2gtk) ----------

#[cfg(target_os = "linux")]
fn install_platform_shim(webview: tauri::webview::PlatformWebview) {
    use glib::object::{Cast, ObjectExt};
    use webkit2gtk::{
        NavigationPolicyDecision, NavigationPolicyDecisionExt, PermissionRequestExt,
        PolicyDecisionExt, PolicyDecisionType, SettingsExt, URIRequestExt, UserContentInjectedFrames,
        UserContentManagerExt, UserMediaPermissionRequest, UserScript, UserScriptInjectionTime,
        WebViewExt,
    };

    let wv = webview.inner();

    // Enable WebRTC / getUserMedia. Off by default on webkit2gtk in Tauri.
    if let Some(settings) = wv.settings() {
        settings.set_enable_media_stream(true);
        settings.set_enable_mediasource(true);
        settings.set_enable_webrtc(true);
    }

    // Auto-grant microphone/camera permission requests.
    wv.connect_permission_request(|_wv, request| {
        if request.is::<UserMediaPermissionRequest>() {
            request.allow();
            true
        } else {
            false
        }
    });

    // Inject the click interceptor into every frame on every page load.
    if let Some(manager) = wv.user_content_manager() {
        let script = UserScript::new(
            CLICK_INTERCEPTOR_JS,
            UserContentInjectedFrames::AllFrames,
            UserScriptInjectionTime::Start,
            &[],
            &[],
        );
        manager.add_script(&script);
    }

    // Belt-and-suspenders fallback for non-click navigations to external URLs.
    // The user script catches clicks; this catches programmatic navigations
    // that bypass the click handler (e.g. `location.href = ...`).
    wv.connect_decide_policy(|_wv, decision, decision_type| {
        if decision_type != PolicyDecisionType::NavigationAction {
            return false;
        }
        let nav_decision: NavigationPolicyDecision =
            match decision.clone().downcast::<NavigationPolicyDecision>() {
                Ok(d) => d,
                Err(_) => return false,
            };
        let Some(action) = nav_decision.navigation_action() else {
            return false;
        };
        let Some(request) = action.request() else {
            return false;
        };
        let Some(uri_gstr) = request.uri() else {
            return false;
        };
        let uri = uri_gstr.to_string();
        if !is_external_url(&uri) {
            return false;
        }
        log::info!("Routing external link to system browser: {uri}");
        if let Err(err) = tauri_plugin_opener::open_url(&uri, None::<&str>) {
            log::warn!("openExternal failed for {uri}: {err}");
        }
        decision.ignore();
        true
    });
}

// ---------- macOS (WKWebView) ----------

#[cfg(target_os = "macos")]
fn install_platform_shim(webview: tauri::webview::PlatformWebview) {
    // `WKUserScript::alloc()` is provided by objc2's `MainThreadOnly` trait.
    // rustc 1.94+ enforces the trait-in-scope rule consistently for method
    // dispatch through inherent-method-on-trait paths; older toolchains let
    // this slide. The trait is re-exported at the crate root in
    // objc2 0.6.x — do NOT use `objc2::top_level_traits::MainThreadOnly`
    // because that module is `mod top_level_traits;` (private), even
    // though one of rustc's E0599 hints will sometimes suggest it.
    use objc2::MainThreadOnly;
    use objc2_foundation::NSString;
    use objc2_web_kit::{
        WKUserContentController, WKUserScript, WKUserScriptInjectionTime,
    };

    // SAFETY: PlatformWebview::controller() returns a non-null
    // WKUserContentController for the lifetime of the window. We only borrow
    // it briefly to call addUserScript. Wry installs its own scripts the
    // exact same way (see wry/src/wkwebview/mod.rs:780-787).
    unsafe {
        let manager: &WKUserContentController = &*webview.controller().cast();
        let alloc = WKUserScript::alloc();
        let source = NSString::from_str(CLICK_INTERCEPTOR_JS);
        let script = WKUserScript::initWithSource_injectionTime_forMainFrameOnly(
            alloc,
            &source,
            WKUserScriptInjectionTime::AtDocumentStart,
            false, // forMainFrameOnly: false → injected into every frame
        );
        manager.addUserScript(&script);
    }
    // Mic/camera permission: wry's WryWebViewUIDelegate already auto-grants
    // every WKMediaCaptureType (see wry/src/wkwebview/class/wry_web_view_ui_delegate.rs).
    // Nothing additional to do here.
}

// ---------- Windows (WebView2) ----------

#[cfg(target_os = "windows")]
fn install_platform_shim(webview: tauri::webview::PlatformWebview) {
    use webview2_com::Microsoft::Web::WebView2::Win32::{
        COREWEBVIEW2_PERMISSION_KIND, COREWEBVIEW2_PERMISSION_KIND_CAMERA,
        COREWEBVIEW2_PERMISSION_KIND_MICROPHONE, COREWEBVIEW2_PERMISSION_STATE_ALLOW,
    };
    use webview2_com::PermissionRequestedEventHandler;
    use windows::core::HSTRING;

    let controller = webview.controller();
    let core = match unsafe { controller.CoreWebView2() } {
        Ok(c) => c,
        Err(err) => {
            log::warn!("WebView2: failed to get CoreWebView2: {err}");
            return;
        }
    };

    // Auto-grant microphone + camera. Clipboard is already auto-granted by wry.
    let mut token = 0i64;
    let handler = PermissionRequestedEventHandler::create(Box::new(|_, args| {
        let Some(args) = args else { return Ok(()) };
        let mut kind = COREWEBVIEW2_PERMISSION_KIND::default();
        unsafe { args.PermissionKind(&mut kind)? };
        if kind == COREWEBVIEW2_PERMISSION_KIND_MICROPHONE
            || kind == COREWEBVIEW2_PERMISSION_KIND_CAMERA
        {
            unsafe { args.SetState(COREWEBVIEW2_PERMISSION_STATE_ALLOW)? };
        }
        Ok(())
    }));
    if let Err(err) = unsafe { core.add_PermissionRequested(&handler, &mut token) } {
        log::warn!("WebView2: add_PermissionRequested failed: {err}");
    }

    // Inject the click interceptor into every frame on every navigation.
    let js = HSTRING::from(CLICK_INTERCEPTOR_JS);
    let result =
        webview2_com::AddScriptToExecuteOnDocumentCreatedCompletedHandler::wait_for_async_operation(
            Box::new(move |handler| unsafe {
                core.AddScriptToExecuteOnDocumentCreated(&js, &handler)
                    .map_err(Into::into)
            }),
            Box::new(|e, _| e),
        );
    if let Err(err) = result {
        log::warn!("WebView2: AddScriptToExecuteOnDocumentCreated failed: {err}");
    }
}

#[cfg(target_os = "linux")]
fn is_external_url(uri: &str) -> bool {
    let lower = uri.to_ascii_lowercase();
    if lower.starts_with("file://")
        || lower.starts_with("tauri://")
        || lower.starts_with("about:")
        || lower.starts_with("data:")
        || lower.starts_with("blob:")
    {
        return false;
    }
    if lower.starts_with("http://localhost")
        || lower.starts_with("http://127.0.0.1")
        || lower.starts_with("https://localhost")
        || lower.starts_with("https://127.0.0.1")
    {
        return false;
    }
    lower.starts_with("http://") || lower.starts_with("https://")
}

#[cfg(test)]
mod tests {
    #[cfg(target_os = "linux")]
    #[test]
    fn classifies_external_urls() {
        use super::is_external_url;
        assert!(!is_external_url("file:///app/index.html"));
        assert!(!is_external_url("tauri://localhost/index.html"));
        assert!(!is_external_url("http://localhost:13305/api/v1/health"));
        assert!(!is_external_url("https://127.0.0.1:8080/foo"));
        assert!(is_external_url("https://lemonade-server.ai/marketplace.html"));
        assert!(is_external_url("http://example.com/"));
        assert!(!is_external_url("about:blank"));
        assert!(!is_external_url("data:text/html,foo"));
    }
}

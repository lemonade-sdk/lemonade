// Port of Electron main.js UDP beacon discovery (lines 503-675).
// Listens on UDP 13305 for "lemonade" service beacons broadcast by the running
// server, extracts the localhost port from the payload URL, and emits a
// `server-port-updated` event to the renderer when it changes.

use serde::Deserialize;
use std::net::{IpAddr, Ipv4Addr, Ipv6Addr};
use std::sync::atomic::{AtomicU16, Ordering};
use tauri::{AppHandle, Emitter};
use tokio::net::UdpSocket;
use tokio::time::{timeout, Duration};

pub const DEFAULT_PORT: u16 = 13305;
pub const BEACON_PORT: u16 = 13305;
const DISCOVERY_TIMEOUT_MS: u64 = 5_000;

pub const SERVER_PORT_UPDATED_EVENT: &str = "server-port-updated";

static CACHED_SERVER_PORT: AtomicU16 = AtomicU16::new(DEFAULT_PORT);

pub fn get_cached_port() -> u16 {
    CACHED_SERVER_PORT.load(Ordering::Relaxed)
}

pub fn set_cached_port(port: u16) {
    CACHED_SERVER_PORT.store(port, Ordering::Relaxed);
}

#[derive(Debug, Deserialize)]
struct BeaconPayload {
    service: String,
    #[allow(dead_code)]
    hostname: Option<String>,
    url: String,
}

fn is_local_address(addr: IpAddr) -> bool {
    match addr {
        IpAddr::V4(ipv4) => ipv4 == Ipv4Addr::LOCALHOST || ipv4.is_loopback(),
        IpAddr::V6(ipv6) => {
            ipv6 == Ipv6Addr::LOCALHOST
                || ipv6.is_loopback()
                || ipv6.to_ipv4_mapped().map(|v| v.is_loopback()).unwrap_or(false)
        }
    }
}

fn is_local_machine(addr: IpAddr) -> bool {
    if is_local_address(addr) {
        return true;
    }
    // Best-effort: also accept any address from our local network interfaces.
    // We reuse std here so we don't need an extra crate.
    let Ok(local_socket) = std::net::UdpSocket::bind("0.0.0.0:0") else {
        return false;
    };
    if local_socket.connect("8.8.8.8:80").is_err() {
        return false;
    }
    match local_socket.local_addr() {
        Ok(local) => local.ip() == addr,
        Err(_) => false,
    }
}

fn parse_port_from_url(url: &str) -> Option<u16> {
    // Example payload: http://1.2.3.4:12345/api/v1/
    // Mirrors main.js regex `/:(\\d+)\\//`
    let after_scheme = url.split("://").nth(1).unwrap_or(url);
    let colon_pos = after_scheme.find(':')?;
    let after_colon = &after_scheme[colon_pos + 1..];
    let slash_pos = after_colon.find('/')?;
    let port_str = &after_colon[..slash_pos];
    port_str.parse::<u16>().ok().filter(|p| *p > 0)
}

// One-shot discovery: bind to BEACON_PORT, wait up to DISCOVERY_TIMEOUT_MS for
// the first valid beacon, return its port. On timeout or error, fall back to
// the cached port (or DEFAULT_PORT).
pub async fn discover_server_port_once() -> u16 {
    match try_discover_server_port().await {
        Some(port) => {
            set_cached_port(port);
            port
        }
        None => {
            let fallback = get_cached_port();
            if fallback == 0 {
                DEFAULT_PORT
            } else {
                fallback
            }
        }
    }
}

async fn try_discover_server_port() -> Option<u16> {
    let socket = match UdpSocket::bind(("0.0.0.0", BEACON_PORT)).await {
        Ok(s) => s,
        Err(err) => {
            log::error!("UDP discovery socket bind failed: {err}");
            return None;
        }
    };

    let _ = socket.set_broadcast(true);

    let deadline = Duration::from_millis(DISCOVERY_TIMEOUT_MS);
    let mut buf = vec![0u8; 2048];

    loop {
        match timeout(deadline, socket.recv_from(&mut buf)).await {
            Ok(Ok((len, addr))) => {
                if !is_local_machine(addr.ip()) {
                    continue;
                }
                if let Some(port) = parse_beacon_message(&buf[..len]) {
                    log::info!("Discovered server port via UDP beacon: {port}");
                    return Some(port);
                }
            }
            Ok(Err(err)) => {
                log::warn!("UDP recv error: {err}");
                return None;
            }
            Err(_) => {
                log::info!("UDP discovery timed out after {DISCOVERY_TIMEOUT_MS}ms");
                return None;
            }
        }
    }
}

fn parse_beacon_message(bytes: &[u8]) -> Option<u16> {
    let text = std::str::from_utf8(bytes).ok()?;
    let payload: BeaconPayload = serde_json::from_str(text).ok()?;
    if payload.service != "lemonade" {
        return None;
    }
    parse_port_from_url(&payload.url)
}

// Background listener: keeps listening for beacons and emits
// `server-port-updated` events when the port changes. Mirrors main.js
// `startBeaconListener` (lines 609-675).
pub async fn run_beacon_listener(app: AppHandle) {
    loop {
        // Skip if an explicit base URL is configured.
        if crate::settings::get_base_url_from_config().is_some() {
            log::info!("Beacon listener skipped - explicit server URL configured");
            tokio::time::sleep(Duration::from_secs(10)).await;
            continue;
        }

        let socket = match UdpSocket::bind(("0.0.0.0", BEACON_PORT)).await {
            Ok(s) => s,
            Err(err) => {
                log::error!("Beacon listener bind failed: {err}, retrying in 10s");
                tokio::time::sleep(Duration::from_secs(10)).await;
                continue;
            }
        };
        let _ = socket.set_broadcast(true);
        log::info!("Beacon listener started on 0.0.0.0:{BEACON_PORT}");

        let mut buf = vec![0u8; 2048];
        loop {
            match socket.recv_from(&mut buf).await {
                Ok((len, addr)) => {
                    if !is_local_machine(addr.ip()) {
                        continue;
                    }
                    if let Some(port) = parse_beacon_message(&buf[..len]) {
                        let cached = get_cached_port();
                        if port != cached {
                            log::info!("Beacon: server port change {cached} -> {port}");
                            set_cached_port(port);
                            let _ = app.emit(SERVER_PORT_UPDATED_EVENT, port);
                        }
                    }
                }
                Err(err) => {
                    log::warn!("Beacon listener recv error: {err}, rebinding in 10s");
                    break;
                }
            }
        }

        tokio::time::sleep(Duration::from_secs(10)).await;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_port_from_url_works() {
        assert_eq!(
            parse_port_from_url("http://192.168.1.1:13305/api/v1/"),
            Some(13305)
        );
        assert_eq!(
            parse_port_from_url("http://127.0.0.1:8080/api/v1/"),
            Some(8080)
        );
        assert_eq!(parse_port_from_url("http://no-port/api/v1/"), None);
    }

    #[test]
    fn parse_beacon_message_accepts_lemonade() {
        let msg = br#"{"service":"lemonade","hostname":"h","url":"http://127.0.0.1:13305/api/v1/"}"#;
        assert_eq!(parse_beacon_message(msg), Some(13305));
    }

    #[test]
    fn parse_beacon_message_rejects_other_service() {
        let msg = br#"{"service":"other","url":"http://127.0.0.1:13305/api/v1/"}"#;
        assert_eq!(parse_beacon_message(msg), None);
    }
}

//! UDP beacon discovery for the running Lemonade server.
//!
//! The server broadcasts a JSON beacon on UDP 13305 describing its HTTP URL.
//! This module provides a one-shot discovery call used by `discover_server_port`
//! and a long-running listener that keeps the cached port in sync and emits a
//! `server-port-updated` Tauri event when the port changes.

use crate::events;
use serde::Deserialize;
use std::net::{IpAddr, Ipv4Addr, Ipv6Addr};
use std::sync::atomic::{AtomicU16, Ordering};
use std::sync::OnceLock;
use tauri::{AppHandle, Emitter};
use tokio::net::UdpSocket;
use tokio::time::{timeout, Duration};

pub(crate) const BEACON_PORT: u16 = 13305;
const DISCOVERY_TIMEOUT_MS: u64 = 5_000;

static CACHED_SERVER_PORT: AtomicU16 = AtomicU16::new(BEACON_PORT);

pub(crate) fn get_cached_port() -> u16 {
    CACHED_SERVER_PORT.load(Ordering::Relaxed)
}

pub(crate) fn set_cached_port(port: u16) {
    CACHED_SERVER_PORT.store(port, Ordering::Relaxed);
}

#[derive(Debug, Deserialize)]
struct BeaconPayload {
    service: String,
    url: String,
}

fn is_loopback(addr: IpAddr) -> bool {
    match addr {
        IpAddr::V4(v4) => v4.is_loopback() || v4 == Ipv4Addr::LOCALHOST,
        IpAddr::V6(v6) => {
            v6.is_loopback()
                || v6 == Ipv6Addr::LOCALHOST
                || v6.to_ipv4_mapped().map(|v| v.is_loopback()).unwrap_or(false)
        }
    }
}

/// Compute the local outbound IP once and cache it. Used to decide whether a
/// beacon from a non-loopback source actually originated on this machine.
fn local_outbound_ip() -> Option<IpAddr> {
    static CELL: OnceLock<Option<IpAddr>> = OnceLock::new();
    *CELL.get_or_init(|| {
        let socket = std::net::UdpSocket::bind("0.0.0.0:0").ok()?;
        socket.connect("8.8.8.8:80").ok()?;
        socket.local_addr().ok().map(|a| a.ip())
    })
}

fn is_local_machine(addr: IpAddr) -> bool {
    is_loopback(addr) || local_outbound_ip() == Some(addr)
}

fn parse_port_from_url(raw: &str) -> Option<u16> {
    url::Url::parse(raw).ok()?.port().filter(|&p| p > 0)
}

fn parse_beacon_message(bytes: &[u8]) -> Option<u16> {
    let payload: BeaconPayload = serde_json::from_slice(bytes).ok()?;
    if payload.service != "lemonade" {
        return None;
    }
    parse_port_from_url(&payload.url)
}

/// One-shot discovery: wait up to `DISCOVERY_TIMEOUT_MS` for a beacon and
/// return the extracted port, or fall back to the cached port.
pub(crate) async fn discover_server_port_once() -> u16 {
    match try_discover_server_port().await {
        Some(port) => {
            set_cached_port(port);
            port
        }
        None => get_cached_port(),
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

/// Background listener: keeps listening for beacons and emits
/// `server-port-updated` when the cached port actually changes. Rebinds the
/// socket after any error and backs off for 10 seconds between retries.
pub(crate) async fn run_beacon_listener(app: AppHandle) {
    loop {
        // Skip if an explicit base URL is configured — no point scraping beacons
        // when the user has told us exactly where to connect.
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
                            let _ = app.emit(events::SERVER_PORT_UPDATED, port);
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

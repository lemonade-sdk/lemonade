//! UDP beacon discovery for the running Lemonade server.
//!
//! The server broadcasts a JSON beacon on UDP 13305 describing its HTTP URL.
//! `run_beacon_listener` is a long-running task started from `lib.rs::setup()`
//! that owns the bound socket for the lifetime of the process: it keeps the
//! cached port in sync and emits a `server-port-updated` Tauri event whenever
//! the port actually changes.
//!
//! In addition to tracking the *local* server's port, the listener also keeps a
//! registry of *every* Lemonade beacon seen on the LAN. Other devices on the
//! same network broadcast the same beacon (see
//! `src/cpp/server/utils/network_beacon.cpp::broadcastThreadLoop`), so the
//! Tauri renderer can offer a "Spotify Connect"-style picker that routes the
//! current LLM chat to a peer device. Entries time out after 10 s without a
//! refresh; on any add/remove/hostname-change the listener emits a
//! `remote-devices-updated` event with the current snapshot.
//!
//! There is intentionally NO one-shot rebind path. Two `UdpSocket::bind` calls
//! to the same port in the same process fail with `EADDRINUSE` on Linux unless
//! `SO_REUSEPORT` is set, so the renderer's `discover_server_port` invoke
//! handler just reads `get_cached_port()` instead of trying to spin up a second
//! listener. The listener is the single source of truth.

use crate::events;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::net::{IpAddr, Ipv4Addr, Ipv6Addr};
use std::sync::atomic::{AtomicU16, Ordering};
use std::sync::{Mutex, OnceLock};
use std::time::Instant;
use tauri::{AppHandle, Emitter};
use tokio::net::UdpSocket;
use tokio::time::Duration;

pub(crate) const BEACON_PORT: u16 = 13305;

/// Drop a remote device from the registry after this long without a refresh.
/// `lemond` broadcasts every 1 s by default, so 10 s gives plenty of headroom
/// for jitter and a couple of dropped packets without being so long that
/// powered-off peers linger forever.
const REMOTE_DEVICE_TTL: Duration = Duration::from_secs(10);

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
    #[serde(default)]
    hostname: String,
    url: String,
}

/// One Lemonade host seen via the LAN beacon. `is_local` is true for the
/// machine running this app (driven by either a loopback beacon or a beacon
/// whose source IP matches our outbound IP). The renderer uses `hostname` for
/// display and `base_url` (with `/api/v1/` already appended) as the target for
/// HTTP requests when the user picks this device for chat.
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct RemoteDevice {
    pub hostname: String,
    #[serde(rename = "baseUrl")]
    pub base_url: String,
    #[serde(rename = "isLocal")]
    pub is_local: bool,
}

#[derive(Debug, Clone)]
struct RemoteDeviceEntry {
    device: RemoteDevice,
    last_seen: Instant,
}

/// Keyed by `base_url` so two interfaces of the same host (e.g. Wi-Fi + Ethernet)
/// show up as separate entries — that matches the beacon broadcaster, which
/// emits one packet per RFC1918 interface, and lets the user pick which network
/// path they want to traverse.
static REMOTE_DEVICES: Mutex<Option<HashMap<String, RemoteDeviceEntry>>> = Mutex::new(None);

/// Last snapshot we emitted via `remote-devices-updated`. Used to suppress
/// no-op updates that would otherwise fire on every received beacon (i.e. once
/// per second per device).
static LAST_EMITTED_SIGNATURE: Mutex<Option<String>> = Mutex::new(None);

fn devices_lock() -> std::sync::MutexGuard<'static, Option<HashMap<String, RemoteDeviceEntry>>> {
    REMOTE_DEVICES.lock().unwrap_or_else(|p| p.into_inner())
}

fn ensure_devices_init(
    map: &mut std::sync::MutexGuard<'static, Option<HashMap<String, RemoteDeviceEntry>>>,
) -> &mut HashMap<String, RemoteDeviceEntry> {
    if map.is_none() {
        **map = Some(HashMap::new());
    }
    map.as_mut().expect("just initialized above")
}

fn snapshot_from(map: &HashMap<String, RemoteDeviceEntry>) -> Vec<RemoteDevice> {
    let mut out: Vec<RemoteDevice> = map.values().map(|e| e.device.clone()).collect();
    // Stable order: locals first (so the renderer can show "This device" at
    // the top without re-sorting), then alphabetical by hostname, then URL as
    // a final tiebreaker for multi-interface hosts.
    out.sort_by(|a, b| {
        b.is_local
            .cmp(&a.is_local)
            .then_with(|| a.hostname.cmp(&b.hostname))
            .then_with(|| a.base_url.cmp(&b.base_url))
    });
    out
}

fn signature_for(devices: &[RemoteDevice]) -> String {
    let mut s = String::new();
    for d in devices {
        s.push_str(&d.base_url);
        s.push('|');
        s.push_str(&d.hostname);
        s.push('|');
        s.push(if d.is_local { 'L' } else { 'R' });
        s.push(';');
    }
    s
}

/// Snapshot of currently-known Lemonade devices on the LAN. Cheap; called by
/// the `list_remote_devices` Tauri command on every renderer mount.
pub fn snapshot_devices() -> Vec<RemoteDevice> {
    let map = devices_lock();
    match map.as_ref() {
        Some(m) => snapshot_from(m),
        None => Vec::new(),
    }
}

/// Insert/refresh a device, prune anything older than `REMOTE_DEVICE_TTL`, and
/// return the new sorted snapshot together with a boolean indicating whether
/// the snapshot's *signature* (set of base_url + hostname + is_local) changed.
fn upsert_and_snapshot(
    base_url: String,
    hostname: String,
    is_local: bool,
    now: Instant,
) -> (Vec<RemoteDevice>, bool) {
    let mut guard = devices_lock();
    let map = ensure_devices_init(&mut guard);

    // Prune stale entries first so a device that just dropped off the LAN
    // disappears as soon as the next beacon arrives.
    map.retain(|_, entry| now.duration_since(entry.last_seen) <= REMOTE_DEVICE_TTL);

    let entry = map.entry(base_url.clone()).or_insert_with(|| RemoteDeviceEntry {
        device: RemoteDevice {
            hostname: hostname.clone(),
            base_url: base_url.clone(),
            is_local,
        },
        last_seen: now,
    });
    // Hostname or local-flag may legitimately change on an interface (e.g.
    // user renames the host); keep them current.
    entry.device.hostname = hostname;
    entry.device.is_local = is_local;
    entry.last_seen = now;

    let snapshot = snapshot_from(map);

    let new_sig = signature_for(&snapshot);
    let mut last = LAST_EMITTED_SIGNATURE.lock().unwrap_or_else(|p| p.into_inner());
    let changed = last.as_deref() != Some(new_sig.as_str());
    if changed {
        *last = Some(new_sig);
    }

    (snapshot, changed)
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

/// Parsed form of a beacon. `port` is `Some` only for the local-machine path
/// that drives `CACHED_SERVER_PORT`; `device` is populated for every valid
/// beacon (local or remote) and feeds the device registry.
struct ParsedBeacon {
    hostname: String,
    url: String,
    port: u16,
}

fn parse_beacon(bytes: &[u8]) -> Option<ParsedBeacon> {
    let payload: BeaconPayload = serde_json::from_slice(bytes).ok()?;
    if payload.service != "lemonade" {
        return None;
    }
    let port = parse_port_from_url(&payload.url)?;
    let hostname = if payload.hostname.is_empty() {
        // Older lemond builds (or test fixtures) may omit hostname. Fall back
        // to the URL host so the renderer always has something to render.
        url::Url::parse(&payload.url)
            .ok()
            .and_then(|u| u.host_str().map(|s| s.to_string()))
            .unwrap_or_else(|| "unknown".to_string())
    } else {
        payload.hostname
    };
    Some(ParsedBeacon {
        hostname,
        url: payload.url,
        port,
    })
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
                    let parsed = match parse_beacon(&buf[..len]) {
                        Some(p) => p,
                        None => continue,
                    };
                    let from_local = is_local_machine(addr.ip());

                    // Local-only path: keep the cached server port in sync.
                    if from_local {
                        let cached = get_cached_port();
                        if parsed.port != cached {
                            log::info!(
                                "Beacon: server port change {cached} -> {}",
                                parsed.port
                            );
                            set_cached_port(parsed.port);
                            let _ = app.emit(events::SERVER_PORT_UPDATED, parsed.port);
                        }
                    }

                    // Registry path: every beacon (local and remote) goes in
                    // so the renderer's "Run on" picker has a uniform list.
                    let (snapshot, changed) = upsert_and_snapshot(
                        parsed.url.clone(),
                        parsed.hostname.clone(),
                        from_local,
                        Instant::now(),
                    );
                    if changed {
                        log::info!(
                            "Beacon registry updated: {} device(s) ({})",
                            snapshot.len(),
                            snapshot
                                .iter()
                                .map(|d| format!(
                                    "{}{}",
                                    d.hostname,
                                    if d.is_local { "*" } else { "" }
                                ))
                                .collect::<Vec<_>>()
                                .join(", ")
                        );
                        let _ = app.emit(events::REMOTE_DEVICES_UPDATED, &snapshot);
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

    fn reset_state() {
        *devices_lock() = Some(HashMap::new());
        *LAST_EMITTED_SIGNATURE.lock().unwrap() = None;
    }

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
    fn parse_beacon_accepts_lemonade_with_hostname() {
        let msg =
            br#"{"service":"lemonade","hostname":"DESKTOP-A","url":"http://127.0.0.1:13305/api/v1/"}"#;
        let parsed = parse_beacon(msg).expect("should parse");
        assert_eq!(parsed.hostname, "DESKTOP-A");
        assert_eq!(parsed.url, "http://127.0.0.1:13305/api/v1/");
        assert_eq!(parsed.port, 13305);
    }

    #[test]
    fn parse_beacon_falls_back_to_url_host_when_hostname_missing() {
        let msg = br#"{"service":"lemonade","url":"http://192.168.1.42:13305/api/v1/"}"#;
        let parsed = parse_beacon(msg).expect("should parse");
        assert_eq!(parsed.hostname, "192.168.1.42");
    }

    #[test]
    fn parse_beacon_rejects_other_service() {
        let msg = br#"{"service":"other","url":"http://127.0.0.1:13305/api/v1/"}"#;
        assert!(parse_beacon(msg).is_none());
    }

    #[test]
    fn upsert_and_snapshot_inserts_and_dedupes() {
        // Test isolation: every test in this module shares the same statics
        // (`REMOTE_DEVICES`, `LAST_EMITTED_SIGNATURE`). Cargo runs tests in
        // parallel by default within a single binary, so anything that touches
        // those statics must serialize via this helper.
        let _guard = TEST_LOCK.lock().unwrap_or_else(|p| p.into_inner());
        reset_state();

        let now = Instant::now();
        let (snap1, changed1) = upsert_and_snapshot(
            "http://192.168.1.10:13305/api/v1/".into(),
            "host-a".into(),
            false,
            now,
        );
        assert!(changed1);
        assert_eq!(snap1.len(), 1);
        assert_eq!(snap1[0].hostname, "host-a");
        assert!(!snap1[0].is_local);

        // Same beacon again → no change reported (we don't want to spam events).
        let (snap2, changed2) = upsert_and_snapshot(
            "http://192.168.1.10:13305/api/v1/".into(),
            "host-a".into(),
            false,
            now,
        );
        assert!(!changed2);
        assert_eq!(snap2.len(), 1);

        // Different host → new entry, change reported.
        let (snap3, changed3) = upsert_and_snapshot(
            "http://127.0.0.1:13305/api/v1/".into(),
            "host-self".into(),
            true,
            now,
        );
        assert!(changed3);
        assert_eq!(snap3.len(), 2);
        // Locals come first in the snapshot.
        assert!(snap3[0].is_local);
        assert_eq!(snap3[0].hostname, "host-self");
    }

    #[test]
    fn upsert_and_snapshot_prunes_stale_entries() {
        let _guard = TEST_LOCK.lock().unwrap_or_else(|p| p.into_inner());
        reset_state();

        let t0 = Instant::now();
        upsert_and_snapshot(
            "http://192.168.1.20:13305/api/v1/".into(),
            "old-host".into(),
            false,
            t0,
        );

        // Fast-forward past TTL by passing a `now` that's later than t0+TTL.
        // upsert_and_snapshot prunes anything older than REMOTE_DEVICE_TTL on
        // every call — so inserting a fresh peer should evict `old-host`.
        let later = t0 + REMOTE_DEVICE_TTL + Duration::from_secs(1);
        let (snap, changed) = upsert_and_snapshot(
            "http://192.168.1.30:13305/api/v1/".into(),
            "new-host".into(),
            false,
            later,
        );
        assert!(changed);
        assert_eq!(snap.len(), 1);
        assert_eq!(snap[0].hostname, "new-host");
    }

    #[test]
    fn upsert_and_snapshot_reports_hostname_change() {
        let _guard = TEST_LOCK.lock().unwrap_or_else(|p| p.into_inner());
        reset_state();

        let now = Instant::now();
        upsert_and_snapshot(
            "http://192.168.1.40:13305/api/v1/".into(),
            "before".into(),
            false,
            now,
        );
        let (snap, changed) = upsert_and_snapshot(
            "http://192.168.1.40:13305/api/v1/".into(),
            "after".into(),
            false,
            now,
        );
        assert!(changed);
        assert_eq!(snap[0].hostname, "after");
    }

    static TEST_LOCK: Mutex<()> = Mutex::new(());
}

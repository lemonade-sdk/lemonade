//! UDP beacon discovery for the running Lemonade server.
//!
//! The server broadcasts a JSON beacon on UDP 13305 describing its HTTP URL.
//! `run_beacon_listener` is a long-running task started from `lib.rs::setup()`
//! that owns the bound socket for the lifetime of the process: it keeps the
//! cached port in sync and emits a `server-port-updated` Tauri event whenever
//! the port actually changes.
//!
//! Beacons from other machines are no longer dropped: they go to
//! `lan_servers` as a list the user chooses from (#1221). Only this machine's
//! own beacons move the cached port, so nothing switches by itself.
//!
//! There is intentionally NO one-shot rebind path. Two `UdpSocket::bind` calls
//! to the same port in the same process fail with `EADDRINUSE` on Linux unless
//! `SO_REUSEPORT` is set, so the renderer's `discover_server_port` invoke
//! handler just reads `get_cached_port()` instead of trying to spin up a second
//! listener. The listener is the single source of truth.

use crate::events;
use crate::lan_servers;
use serde::Deserialize;
use std::net::{IpAddr, Ipv4Addr, Ipv6Addr};
use std::sync::atomic::{AtomicU16, Ordering};
use std::sync::OnceLock;
use tauri::{AppHandle, Emitter};
use tokio::net::UdpSocket;
use tokio::time::{Duration, Instant};

pub(crate) const BEACON_PORT: u16 = 13305;

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

fn own_hostname() -> &'static str {
    static CELL: OnceLock<String> = OnceLock::new();
    CELL.get_or_init(|| gethostname::gethostname().to_string_lossy().trim().to_lowercase())
}

/// lemond sends one beacon per RFC 1918 interface, so this machine's own beacons
/// also arrive from its other interfaces' addresses. The hostname catches those.
fn is_local_machine(addr: IpAddr, hostname: &str) -> bool {
    is_loopback(addr)
        || local_outbound_ip() == Some(addr)
        || (!hostname.trim().is_empty() && hostname.trim().to_lowercase() == own_hostname())
}

/// Bind the beacon port with address reuse, so the app shares UDP 13305 with
/// other listeners on the same machine (`lemonade scan`, a second app, lemond)
/// instead of failing to bind. On macOS a plain bind fails outright when the
/// port is already held, and the app then never learns the server's port.
fn bind_beacon_socket(port: u16) -> std::io::Result<UdpSocket> {
    use socket2::{Domain, Protocol, Socket, Type};
    let s = Socket::new(Domain::IPV4, Type::DGRAM, Some(Protocol::UDP))?;
    s.set_reuse_address(true)?;
    #[cfg(all(unix, not(any(target_os = "solaris", target_os = "illumos"))))]
    s.set_reuse_port(true)?;
    s.set_broadcast(true)?;
    s.set_nonblocking(true)?;
    s.bind(&std::net::SocketAddr::from((Ipv4Addr::UNSPECIFIED, port)).into())?;
    UdpSocket::from_std(s.into())
}

fn parse_port_from_url(raw: &str) -> Option<u16> {
    url::Url::parse(raw).ok()?.port().filter(|&p| p > 0)
}

fn parse_beacon(bytes: &[u8]) -> Option<BeaconPayload> {
    let payload: BeaconPayload = serde_json::from_slice(bytes).ok()?;
    (payload.service == "lemonade").then_some(payload)
}

#[cfg(test)]
fn parse_beacon_message(bytes: &[u8]) -> Option<u16> {
    parse_port_from_url(&parse_beacon(bytes)?.url)
}

/// What the listener does with one beacon.
#[derive(Debug, PartialEq, Eq)]
enum BeaconAction {
    /// This machine's server, no explicit URL: keep the cached port in sync.
    SyncPort(u16),
    /// This machine's server, but the user chose a server URL (or the URL has no port).
    Ignore,
    /// Another machine's server: goes on the LAN list.
    RecordLanServer,
}

/// The listener stays up when an explicit server URL is set, so the LAN list
/// still fills; the explicit URL only stops local beacons from moving the port.
fn beacon_action(local: bool, explicit_url: bool, url: &str) -> BeaconAction {
    if !local {
        return BeaconAction::RecordLanServer;
    }
    if explicit_url {
        return BeaconAction::Ignore;
    }
    parse_port_from_url(url).map_or(BeaconAction::Ignore, BeaconAction::SyncPort)
}

/// Background listener. This machine's beacons keep the cached port in sync
/// (emitting `server-port-updated` when it changes, unless an explicit server
/// URL is configured); other machines' beacons update the LAN server list
/// (emitting `lan-servers-updated` when a server appears, gains a URL or goes
/// quiet). Rebinds after any error, backing off 10 seconds.
pub(crate) async fn run_beacon_listener(app: AppHandle) {
    loop {
        let socket = match bind_beacon_socket(BEACON_PORT) {
            Ok(s) => s,
            Err(err) => {
                log::error!("Beacon listener bind failed: {err}, retrying in 10s");
                tokio::time::sleep(Duration::from_secs(10)).await;
                continue;
            }
        };
        log::info!("Beacon listener started on 0.0.0.0:{BEACON_PORT}");

        let mut buf = vec![0u8; 2048];
        let mut tick = tokio::time::interval(Duration::from_secs(5));
        loop {
            tokio::select! {
                got = socket.recv_from(&mut buf) => match got {
                    Ok((len, addr)) => {
                        let Some(beacon) = parse_beacon(&buf[..len]) else { continue };
                        let local = is_local_machine(addr.ip(), &beacon.hostname);
                        let explicit = crate::settings::get_base_url_from_config().is_some();
                        match beacon_action(local, explicit, &beacon.url) {
                            BeaconAction::SyncPort(port) => {
                                let cached = get_cached_port();
                                if port != cached {
                                    log::info!("Beacon: server port change {cached} -> {port}");
                                    set_cached_port(port);
                                    let _ = app.emit(events::SERVER_PORT_UPDATED, port);
                                }
                            }
                            BeaconAction::Ignore => {}
                            BeaconAction::RecordLanServer => {
                                let changed = lan_servers::with_registry(|r| {
                                    r.record(&beacon.hostname, &beacon.url, addr.ip(), Instant::now().into_std())
                                });
                                if changed {
                                    log::info!("Beacon: Lemonade on the network: {} ({})", beacon.hostname, beacon.url);
                                    let _ = app.emit(events::LAN_SERVERS_UPDATED, ());
                                }
                            }
                        }
                    }
                    Err(err) => {
                        log::warn!("Beacon listener recv error: {err}, rebinding in 10s");
                        break;
                    }
                },
                _ = tick.tick() => {
                    if lan_servers::with_registry(|r| r.expire(Instant::now().into_std())) {
                        let _ = app.emit(events::LAN_SERVERS_UPDATED, ());
                    }
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
    fn beacon_hostname_is_kept_and_optional() {
        let b = parse_beacon(br#"{"service":"lemonade","hostname":"c4","url":"http://10.44.0.14:13305/api/v1/"}"#).unwrap();
        assert_eq!(b.hostname, "c4");
        let old = parse_beacon(br#"{"service":"lemonade","url":"http://10.44.0.14:13305/api/v1/"}"#).unwrap();
        assert_eq!(old.hostname, "");
    }

    #[test]
    fn this_machines_hostname_counts_as_local() {
        let me = own_hostname().to_string();
        let lan: IpAddr = "192.0.2.50".parse().unwrap(); // TEST-NET, never this machine
        assert!(is_local_machine(lan, &me.to_uppercase()));
        assert!(!is_local_machine(lan, "some-other-host-1221"));
        assert!(!is_local_machine(lan, ""));
        assert!(is_local_machine("127.0.0.1".parse().unwrap(), "anything"));
    }

    #[test]
    fn explicit_url_stops_port_sync_but_not_the_lan_list() {
        let url = "http://192.0.2.50:13305/api/v1/";
        // another machine's beacon reaches the LAN list with or without an explicit URL
        assert_eq!(beacon_action(false, true, url), BeaconAction::RecordLanServer);
        assert_eq!(beacon_action(false, false, url), BeaconAction::RecordLanServer);
        // this machine's beacon moves the port only when no URL was chosen
        assert_eq!(beacon_action(true, false, url), BeaconAction::SyncPort(13305));
        assert_eq!(beacon_action(true, true, url), BeaconAction::Ignore);
        assert_eq!(beacon_action(true, false, "not a url"), BeaconAction::Ignore);
    }

    #[test]
    fn two_listeners_can_share_the_beacon_port() {
        let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap();
        rt.block_on(async {
            // Port 0 first, then the port it was given: the test never touches the real beacon port,
            // which a running app or lemond may hold on the machine running the tests.
            let a = bind_beacon_socket(0).expect("first bind");
            let port = a.local_addr().expect("local addr").port();
            let b = bind_beacon_socket(port);
            assert!(b.is_ok(), "second bind on {port}: {:?}", b.err());
        });
    }

    #[test]
    fn parse_beacon_message_rejects_other_service() {
        let msg = br#"{"service":"other","url":"http://127.0.0.1:13305/api/v1/"}"#;
        assert_eq!(parse_beacon_message(msg), None);
    }
}

//! Lemonade servers found on the local network (#1221).
//!
//! The beacon listener (`beacon.rs`) used to drop every beacon that did not come
//! from this machine, because "last beacon wins" would switch servers behind the
//! user's back. This module keeps those beacons instead, as a list the user
//! chooses from, and never switches by itself:
//!
//! 1. Several beacons: one row per server (grouped by the beacon's hostname),
//!    with every URL it announces.
//! 2. A beacon that is not mine: the user can Ignore it; ignored servers stay
//!    hidden unless the list is asked for them.
//! 3. A local server plus LAN servers: the local one stays the default; a LAN
//!    server is only used after the user presses Use on it.
//!
//! Each server also gets a zone from the address its beacon came from:
//! `home` (RFC 1918, link-local, IPv6 ULA), `tailscale` (100.64.0.0/10,
//! fd7a:115c:a1e0::/48) or `away` (anything else). The zone is shown to the user
//! and is the hook for later trust rules; it does not gate anything yet.
//!
//! The user's choices (Use / Ignore) are kept in `lan_servers.json` next to the
//! app settings, keyed by the same id, so they survive restarts and a server is
//! still listed (as not seen) while it is offline.

use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, BTreeSet};
use std::net::IpAddr;
use std::path::PathBuf;
use std::sync::Mutex;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

/// A server whose beacon has not been heard for this long is no longer "seen".
/// lemond beacons every 2 seconds.
pub(crate) const SEEN_FOR: Duration = Duration::from_secs(30);

const CHOICES_FILE_NAME: &str = "lan_servers.json";

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub(crate) enum Zone {
    Home,
    Tailscale,
    Away,
}

pub(crate) fn zone_of(addr: IpAddr) -> Zone {
    match addr {
        IpAddr::V4(v4) => {
            let o = v4.octets();
            if o[0] == 100 && (o[1] & 0xC0) == 64 {
                Zone::Tailscale
            } else if v4.is_private() || v4.is_link_local() {
                Zone::Home
            } else {
                Zone::Away
            }
        }
        IpAddr::V6(v6) => {
            if let Some(v4) = v6.to_ipv4_mapped() {
                return zone_of(IpAddr::V4(v4));
            }
            let s = v6.segments();
            if s[0] == 0xfd7a && s[1] == 0x115c && s[2] == 0xa1e0 {
                Zone::Tailscale
            } else if (s[0] & 0xfe00) == 0xfc00 || (s[0] & 0xffc0) == 0xfe80 {
                Zone::Home
            } else {
                Zone::Away
            }
        }
    }
}

/// The user's choice for one server.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub(crate) enum Choice {
    Accepted,
    Ignored,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub(crate) struct SavedChoice {
    pub choice: Choice,
    /// Hostname and URL at the time of the choice, so an offline server can still be listed.
    #[serde(default)]
    pub hostname: String,
    #[serde(default)]
    pub url: String,
    #[serde(default)]
    pub updated: u64,
}

#[derive(Debug, Default, Serialize, Deserialize)]
struct ChoicesFile {
    #[serde(default)]
    servers: BTreeMap<String, SavedChoice>,
}

struct Seen {
    hostname: String,
    urls: BTreeSet<String>,
    zone: Zone,
    last: Instant,
}

/// One row of the list the renderer shows.
#[derive(Debug, Clone, Serialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub(crate) struct LanServer {
    pub id: String,
    pub hostname: String,
    pub urls: Vec<String>,
    /// `None` when the server is only known from a saved choice and has not been heard.
    pub zone: Option<Zone>,
    pub seen: bool,
    pub seconds_since_seen: Option<u64>,
    /// "new", "accepted" or "ignored".
    pub state: &'static str,
}

#[derive(Default)]
pub(crate) struct Registry {
    seen: BTreeMap<String, Seen>,
}

/// The id a beacon is filed under: its hostname (lowercased), or the sender's address when it has none.
/// v1 beacons carry no node identity; two machines with the same hostname share a row until beacon v2.
pub(crate) fn server_id(hostname: &str, from: IpAddr) -> String {
    let h = hostname.trim().to_lowercase();
    if h.is_empty() {
        from.to_string()
    } else {
        h
    }
}

impl Registry {
    /// Record one beacon from another machine. Returns true when the list changed (a new server or a new URL),
    /// not on every repeat beacon, so the renderer is only told about real changes.
    pub(crate) fn record(&mut self, hostname: &str, url: &str, from: IpAddr, now: Instant) -> bool {
        let id = server_id(hostname, from);
        let zone = zone_of(from);
        match self.seen.get_mut(&id) {
            Some(s) => {
                let was_expired = now.duration_since(s.last) >= SEEN_FOR;
                s.last = now;
                // A server reachable on any home address counts as home.
                let zone_changed = zone == Zone::Home && s.zone != Zone::Home;
                if zone_changed {
                    s.zone = zone;
                }
                s.urls.insert(url.to_string()) || was_expired || zone_changed
            }
            None => {
                let display = if hostname.trim().is_empty() { from.to_string() } else { hostname.trim().to_string() };
                self.seen.insert(
                    id,
                    Seen { hostname: display, urls: BTreeSet::from([url.to_string()]), zone, last: now },
                );
                true
            }
        }
    }

    /// Forget servers not heard for `SEEN_FOR`. Returns true when one was dropped.
    pub(crate) fn expire(&mut self, now: Instant) -> bool {
        let before = self.seen.len();
        self.seen.retain(|_, s| now.duration_since(s.last) < SEEN_FOR);
        self.seen.len() != before
    }

    /// The rows: every seen server, plus saved choices for servers not seen right now. New first, then
    /// accepted, then ignored; by hostname within each.
    pub(crate) fn list(&self, saved: &BTreeMap<String, SavedChoice>, now: Instant) -> Vec<LanServer> {
        let mut rows: Vec<LanServer> = Vec::new();
        for (id, s) in &self.seen {
            let state = match saved.get(id).map(|c| c.choice) {
                Some(Choice::Accepted) => "accepted",
                Some(Choice::Ignored) => "ignored",
                None => "new",
            };
            rows.push(LanServer {
                id: id.clone(),
                hostname: s.hostname.clone(),
                urls: s.urls.iter().cloned().collect(),
                zone: Some(s.zone),
                seen: true,
                seconds_since_seen: Some(now.duration_since(s.last).as_secs()),
                state,
            });
        }
        for (id, c) in saved {
            if self.seen.contains_key(id) {
                continue;
            }
            rows.push(LanServer {
                id: id.clone(),
                hostname: if c.hostname.is_empty() { id.clone() } else { c.hostname.clone() },
                urls: if c.url.is_empty() { vec![] } else { vec![c.url.clone()] },
                zone: None,
                seen: false,
                seconds_since_seen: None,
                state: match c.choice {
                    Choice::Accepted => "accepted",
                    Choice::Ignored => "ignored",
                },
            });
        }
        let rank = |s: &str| match s {
            "new" => 0,
            "accepted" => 1,
            _ => 2,
        };
        rows.sort_by(|a, b| rank(a.state).cmp(&rank(b.state)).then_with(|| a.hostname.cmp(&b.hostname)));
        rows
    }
}

pub(crate) static REGISTRY: Mutex<Option<Registry>> = Mutex::new(None);

pub(crate) fn with_registry<R>(f: impl FnOnce(&mut Registry) -> R) -> R {
    let mut g = REGISTRY.lock().unwrap_or_else(|e| e.into_inner());
    f(g.get_or_insert_with(Registry::default))
}

// ---------- Saved choices ----------

fn choices_path() -> Option<PathBuf> {
    Some(dirs::config_dir()?.join("lemonade").join(CHOICES_FILE_NAME))
}

pub(crate) fn read_choices() -> BTreeMap<String, SavedChoice> {
    choices_path()
        .and_then(|p| std::fs::read_to_string(p).ok())
        .and_then(|s| serde_json::from_str::<ChoicesFile>(&s).ok())
        .map(|f| f.servers)
        .unwrap_or_default()
}

fn write_choices(servers: &BTreeMap<String, SavedChoice>) -> Result<(), String> {
    let path = choices_path().ok_or("no config directory")?;
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir).map_err(|e| e.to_string())?;
    }
    let body = serde_json::to_string_pretty(&ChoicesFile { servers: servers.clone() }).map_err(|e| e.to_string())?;
    let tmp = path.with_extension("json.tmp");
    std::fs::write(&tmp, body).map_err(|e| e.to_string())?;
    std::fs::rename(&tmp, &path).map_err(|e| e.to_string())
}

/// Apply a choice to a saved-choices map: "accepted", "ignored", or "new" (forget the choice).
pub(crate) fn apply_choice(
    servers: &mut BTreeMap<String, SavedChoice>,
    id: &str,
    state: &str,
    hostname: &str,
    url: &str,
) -> Result<(), String> {
    let choice = match state {
        "accepted" => Some(Choice::Accepted),
        "ignored" => Some(Choice::Ignored),
        "new" => None,
        other => return Err(format!("unknown server state {other:?}")),
    };
    match choice {
        None => {
            servers.remove(id);
        }
        Some(choice) => {
            let updated = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
            servers.insert(id.to_string(), SavedChoice { choice, hostname: hostname.to_string(), url: url.to_string(), updated });
        }
    }
    Ok(())
}

pub(crate) fn set_choice(id: &str, state: &str, url: Option<&str>) -> Result<(), String> {
    let mut servers = read_choices();
    let (hostname, first_url) = with_registry(|r| {
        r.seen
            .get(id)
            .map(|s| (s.hostname.clone(), s.urls.iter().next().cloned().unwrap_or_default()))
            .unwrap_or_default()
    });
    let hostname = if hostname.is_empty() { servers.get(id).map(|c| c.hostname.clone()).unwrap_or_default() } else { hostname };
    let url = url.map(str::to_string).filter(|u| !u.is_empty()).unwrap_or(first_url);
    apply_choice(&mut servers, id, state, &hostname, &url)?;
    write_choices(&servers)
}

pub(crate) fn list_now() -> Vec<LanServer> {
    let saved = read_choices();
    with_registry(|r| r.list(&saved, Instant::now()))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::{Ipv4Addr, Ipv6Addr};

    fn v4(a: u8, b: u8, c: u8, d: u8) -> IpAddr {
        IpAddr::V4(Ipv4Addr::new(a, b, c, d))
    }

    #[test]
    fn zones_follow_the_address() {
        assert_eq!(zone_of(v4(192, 168, 4, 29)), Zone::Home);
        assert_eq!(zone_of(v4(10, 44, 0, 14)), Zone::Home);
        assert_eq!(zone_of(v4(172, 20, 1, 1)), Zone::Home);
        assert_eq!(zone_of(v4(169, 254, 3, 3)), Zone::Home);
        assert_eq!(zone_of(v4(100, 72, 182, 77)), Zone::Tailscale);
        assert_eq!(zone_of(v4(100, 128, 0, 1)), Zone::Away); // just outside 100.64.0.0/10
        assert_eq!(zone_of(v4(8, 8, 8, 8)), Zone::Away);
        assert_eq!(zone_of("fd7a:115c:a1e0::1".parse().unwrap()), Zone::Tailscale);
        assert_eq!(zone_of("fd00::5".parse().unwrap()), Zone::Home);
        assert_eq!(zone_of("fe80::1".parse().unwrap()), Zone::Home);
        assert_eq!(zone_of(IpAddr::V6(Ipv4Addr::new(192, 168, 1, 2).to_ipv6_mapped())), Zone::Home);
        assert_eq!(zone_of(IpAddr::V6(Ipv6Addr::new(0x2001, 0xdb8, 0, 0, 0, 0, 0, 1))), Zone::Away);
    }

    #[test]
    fn one_row_per_host_with_every_url() {
        let mut r = Registry::default();
        let t = Instant::now();
        assert!(r.record("c4", "http://10.44.0.14:13305/api/v1/", v4(10, 44, 0, 14), t));
        assert!(r.record("C4", "http://192.168.4.30:13305/api/v1/", v4(192, 168, 4, 30), t));
        // The same beacon again is not a change.
        assert!(!r.record("c4", "http://10.44.0.14:13305/api/v1/", v4(10, 44, 0, 14), t));
        assert!(r.record("t440", "http://192.168.4.29:13305/api/v1/", v4(192, 168, 4, 29), t));
        let rows = r.list(&BTreeMap::new(), t);
        assert_eq!(rows.len(), 2);
        assert_eq!(rows[0].id, "c4");
        assert_eq!(rows[0].urls.len(), 2);
        assert_eq!(rows[0].state, "new");
        assert_eq!(rows[0].zone, Some(Zone::Home));
    }

    #[test]
    fn silent_servers_expire_and_come_back() {
        let mut r = Registry::default();
        let t = Instant::now();
        r.record("c1", "http://192.168.4.174:13305/api/v1/", v4(192, 168, 4, 174), t);
        assert!(!r.expire(t + Duration::from_secs(10)));
        assert!(r.expire(t + SEEN_FOR + Duration::from_secs(1)));
        assert!(r.list(&BTreeMap::new(), t).is_empty());
        assert!(r.record("c1", "http://192.168.4.174:13305/api/v1/", v4(192, 168, 4, 174), t + SEEN_FOR * 2));
    }

    #[test]
    fn choices_label_rows_and_keep_offline_servers() {
        let mut r = Registry::default();
        let t = Instant::now();
        r.record("c2", "http://192.168.4.23:13305/api/v1/", v4(192, 168, 4, 23), t);
        r.record("guest-laptop", "http://192.168.4.99:13305/api/v1/", v4(192, 168, 4, 99), t);
        let mut saved = BTreeMap::new();
        apply_choice(&mut saved, "guest-laptop", "ignored", "guest-laptop", "").unwrap();
        apply_choice(&mut saved, "c4", "accepted", "c4", "http://10.44.0.14:13305/api/v1/").unwrap();
        let rows = r.list(&saved, t);
        let states: Vec<_> = rows.iter().map(|x| (x.id.as_str(), x.state, x.seen)).collect();
        assert_eq!(states, vec![("c2", "new", true), ("c4", "accepted", false), ("guest-laptop", "ignored", true)]);
        apply_choice(&mut saved, "guest-laptop", "new", "", "").unwrap();
        assert!(!saved.contains_key("guest-laptop"));
        assert!(apply_choice(&mut saved, "x", "bogus", "", "").is_err());
    }

    #[test]
    fn a_beacon_without_hostname_is_filed_by_address() {
        assert_eq!(server_id("  ", v4(192, 168, 4, 5)), "192.168.4.5");
        assert_eq!(server_id("NautiK9", v4(192, 168, 4, 5)), "nautik9");
    }

    #[test]
    fn choices_file_round_trips_and_tolerates_missing_fields() {
        let f: ChoicesFile = serde_json::from_str(r#"{"servers":{"c4":{"choice":"accepted"}}}"#).unwrap();
        assert_eq!(f.servers["c4"].choice, Choice::Accepted);
        let empty: ChoicesFile = serde_json::from_str("{}").unwrap();
        assert!(empty.servers.is_empty());
    }
}

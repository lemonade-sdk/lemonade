//! End-to-end test for virtual UDP: two netstacks wired back-to-back at the IP
//! layer, each with a bound virtual UDP port delivering to an in-process
//! callback. No kernel socket carries the datagrams.

use std::ffi::{c_void, CString};
use std::sync::{Condvar, Mutex};
use std::time::Duration;

use lemonade_virtual_netstack::*;

struct Wiring {
    peer: Mutex<Option<*mut NsHandle>>,
}
unsafe impl Send for Wiring {}
unsafe impl Sync for Wiring {}

unsafe extern "C" fn forward_to_peer(ctx: *mut c_void, pkt: *const u8, len: usize) {
    let wiring = &*(ctx as *const Wiring);
    if let Some(peer) = *wiring.peer.lock().unwrap() {
        ns_feed_inbound(peer, pkt, len);
    }
}

#[derive(Debug, Clone, PartialEq)]
struct Received {
    src_ip: u32,
    src_port: u16,
    dst_ip: u32,
    dst_port: u16,
    data: Vec<u8>,
}

#[derive(Default)]
struct Inbox {
    got: Mutex<Vec<Received>>,
    cv: Condvar,
}

impl Inbox {
    fn wait_for(&self, n: usize) -> Vec<Received> {
        let guard = self.got.lock().unwrap();
        let (guard, _) = self
            .cv
            .wait_timeout_while(guard, Duration::from_secs(5), |g| g.len() < n)
            .unwrap();
        guard.clone()
    }
}

unsafe extern "C" fn on_datagram(
    ctx: *mut c_void,
    src_ip: u32,
    src_port: u16,
    dst_ip: u32,
    dst_port: u16,
    data: *const u8,
    len: usize,
) {
    let inbox = &*(ctx as *const Inbox);
    inbox.got.lock().unwrap().push(Received {
        src_ip,
        src_port,
        dst_ip,
        dst_port,
        data: std::slice::from_raw_parts(data, len).to_vec(),
    });
    inbox.cv.notify_all();
}

fn cs(s: &str) -> CString {
    CString::new(s).unwrap()
}

const A_IP: u32 = 0xAC10_0001; // 172.16.0.1
const B_IP: u32 = 0xAC10_0002; // 172.16.0.2

struct Pair {
    a: *mut NsHandle,
    b: *mut NsHandle,
    wiring_a: *mut Wiring,
    wiring_b: *mut Wiring,
}

impl Pair {
    fn new() -> Pair {
        let wiring_a = Box::into_raw(Box::new(Wiring { peer: Mutex::new(None) }));
        let wiring_b = Box::into_raw(Box::new(Wiring { peer: Mutex::new(None) }));
        let a = unsafe { ns_create(1420, forward_to_peer, wiring_a as *mut c_void) };
        let b = unsafe { ns_create(1420, forward_to_peer, wiring_b as *mut c_void) };
        assert!(!a.is_null() && !b.is_null());
        unsafe {
            *(*wiring_a).peer.lock().unwrap() = Some(b);
            *(*wiring_b).peer.lock().unwrap() = Some(a);
            assert_eq!(ns_add_local_ip(a, cs("172.16.0.1/22").as_ptr()), 0);
            assert_eq!(ns_add_local_ip(b, cs("172.16.0.2/22").as_ptr()), 0);
        }
        Pair { a, b, wiring_a, wiring_b }
    }
}

impl Drop for Pair {
    fn drop(&mut self) {
        unsafe {
            ns_destroy(self.a);
            ns_destroy(self.b);
            drop(Box::from_raw(self.wiring_a));
            drop(Box::from_raw(self.wiring_b));
        }
    }
}

#[test]
fn datagrams_round_trip_between_bound_ports() {
    let pair = Pair::new();
    let inbox_a = Box::new(Inbox::default());
    let inbox_b = Box::new(Inbox::default());
    unsafe {
        let ctx_a = &*inbox_a as *const Inbox as *mut c_void;
        let ctx_b = &*inbox_b as *const Inbox as *mut c_void;
        assert_eq!(ns_udp_bind(pair.a, cs("172.16.0.1").as_ptr(), 9102, on_datagram, ctx_a), 0);
        assert_eq!(ns_udp_bind(pair.b, cs("172.16.0.2").as_ptr(), 9102, on_datagram, ctx_b), 0);

        let msg = b"digest from a";
        assert_eq!(
            ns_udp_send(pair.a, cs("172.16.0.1").as_ptr(), 9102,
                        cs("172.16.0.2").as_ptr(), 9102, msg.as_ptr(), msg.len()),
            0
        );
    }

    let got = inbox_b.wait_for(1);
    assert_eq!(
        got,
        vec![Received {
            src_ip: A_IP,
            src_port: 9102,
            dst_ip: B_IP,
            dst_port: 9102,
            data: b"digest from a".to_vec(),
        }]
    );

    // The reply goes back to the observed source.
    unsafe {
        let reply = b"request from b";
        assert_eq!(
            ns_udp_send(pair.b, cs("172.16.0.2").as_ptr(), 9102,
                        cs("172.16.0.1").as_ptr(), got[0].src_port, reply.as_ptr(), reply.len()),
            0
        );
    }
    let back = inbox_a.wait_for(1);
    assert_eq!(back.len(), 1);
    assert_eq!(back[0].src_ip, B_IP);
    assert_eq!(back[0].data, b"request from b".to_vec());

    drop(pair);
}

#[test]
fn oversize_unbound_and_malformed_sends_are_refused_and_large_ones_fragment() {
    let pair = Pair::new();
    let inbox_b = Box::new(Inbox::default());
    unsafe {
        let ctx_b = &*inbox_b as *const Inbox as *mut c_void;
        assert_eq!(ns_udp_bind(pair.b, cs("172.16.0.2").as_ptr(), 9102, on_datagram, ctx_b), 0);

        // One byte over the largest IPv4 UDP payload.
        let big = vec![0u8; NS_MAX_UDP_PAYLOAD + 1];
        assert_eq!(
            ns_udp_send(pair.a, cs("172.16.0.1").as_ptr(), 9102,
                        cs("172.16.0.2").as_ptr(), 9102, big.as_ptr(), big.len()),
            -1
        );
        let msg = b"x";
        assert_eq!(
            ns_udp_send(pair.a, cs("not-an-ip").as_ptr(), 9102,
                        cs("172.16.0.2").as_ptr(), 9102, msg.as_ptr(), msg.len()),
            -1
        );
        assert_eq!(
            ns_udp_send(pair.a, cs("172.16.0.1").as_ptr(), 0,
                        cs("172.16.0.2").as_ptr(), 9102, msg.as_ptr(), msg.len()),
            -1
        );
        assert_eq!(ns_udp_bind(pair.a, cs("172.16.0.1").as_ptr(), 0, on_datagram, ctx_b), -1);

        // A has no binding on 172.16.0.1:9102, so this is queued then dropped.
        assert_eq!(
            ns_udp_send(pair.a, cs("172.16.0.1").as_ptr(), 9102,
                        cs("172.16.0.2").as_ptr(), 9102, msg.as_ptr(), msg.len()),
            0
        );

        // Positive control: a datagram far larger than the MTU arrives intact
        // (fragmented and reassembled) once A binds the source endpoint.
        let ctx_none = &*inbox_b as *const Inbox as *mut c_void;
        assert_eq!(ns_udp_bind(pair.a, cs("172.16.0.1").as_ptr(), 9102, on_datagram, ctx_none), 0);
        let fits: Vec<u8> = (0..60_000u32).map(|i| (i % 251) as u8).collect();
        assert_eq!(
            ns_udp_send(pair.a, cs("172.16.0.1").as_ptr(), 9102,
                        cs("172.16.0.2").as_ptr(), 9102, fits.as_ptr(), fits.len()),
            0
        );
    }
    let got = inbox_b.wait_for(1);
    assert_eq!(got.len(), 1, "only the bound, in-limit datagram should arrive");
    let expected: Vec<u8> = (0..60_000u32).map(|i| (i % 251) as u8).collect();
    assert_eq!(got[0].data, expected, "fragmented datagram was not reassembled intact");

    drop(pair);
}

#[test]
fn burst_larger_than_one_poll_is_delivered_in_full() {
    let pair = Pair::new();
    let inbox_b = Box::new(Inbox::default());
    let inbox_a = Box::new(Inbox::default());
    const BURST: usize = 300;
    unsafe {
        let ctx_a = &*inbox_a as *const Inbox as *mut c_void;
        let ctx_b = &*inbox_b as *const Inbox as *mut c_void;
        assert_eq!(ns_udp_bind(pair.a, cs("172.16.0.1").as_ptr(), 9102, on_datagram, ctx_a), 0);
        assert_eq!(ns_udp_bind(pair.b, cs("172.16.0.2").as_ptr(), 9102, on_datagram, ctx_b), 0);
        let src = cs("172.16.0.1");
        let dst = cs("172.16.0.2");
        for i in 0..BURST {
            let msg = vec![(i % 251) as u8; 600];
            assert_eq!(
                ns_udp_send(pair.a, src.as_ptr(), 9102, dst.as_ptr(), 9102, msg.as_ptr(), msg.len()),
                0
            );
        }
    }
    let got = inbox_b.wait_for(BURST);
    assert_eq!(got.len(), BURST, "a catch-up sized burst lost datagrams");
    drop(pair);
}

#[test]
fn maximum_size_datagrams_in_series_all_arrive() {
    let pair = Pair::new();
    let inbox_a = Box::new(Inbox::default());
    let inbox_b = Box::new(Inbox::default());
    const COUNT: usize = 6;
    unsafe {
        let ctx_a = &*inbox_a as *const Inbox as *mut c_void;
        let ctx_b = &*inbox_b as *const Inbox as *mut c_void;
        assert_eq!(ns_udp_bind(pair.a, cs("172.16.0.1").as_ptr(), 9102, on_datagram, ctx_a), 0);
        assert_eq!(ns_udp_bind(pair.b, cs("172.16.0.2").as_ptr(), 9102, on_datagram, ctx_b), 0);
        let src = cs("172.16.0.1");
        let dst = cs("172.16.0.2");
        for i in 0..COUNT {
            let msg = vec![i as u8; NS_MAX_UDP_PAYLOAD];
            assert_eq!(
                ns_udp_send(pair.a, src.as_ptr(), 9102, dst.as_ptr(), 9102, msg.as_ptr(), msg.len()),
                0
            );
        }
    }
    let got = inbox_b.wait_for(COUNT);
    assert_eq!(got.len(), COUNT, "sizes: {:?}", got.iter().map(|r| r.data.len()).collect::<Vec<_>>());
    for (i, r) in got.iter().enumerate() {
        assert_eq!(r.data.len(), NS_MAX_UDP_PAYLOAD);
        assert!(r.data.iter().all(|b| *b == i as u8), "datagram {i} corrupted or reordered");
    }
    drop(pair);
}

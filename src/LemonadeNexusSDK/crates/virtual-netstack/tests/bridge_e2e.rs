//! End-to-end test: a real TCP client -> stack A (egress) -> IP packets -> stack
//! B (ingress) -> a real echo server, all over loopback with no privileges and
//! no kernel network interface.
//!
//! Two netstacks are wired back-to-back at the IP layer: every packet A emits
//! is fed to B and vice versa. This exercises the full smoltcp TCP path through
//! the crate in both directions.

use std::ffi::{c_void, CString};
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::Mutex;
use std::time::Duration;

use lemonade_virtual_netstack::*;

/// Bridges packets emitted by one stack into another stack's inbound queue.
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

fn cs(s: &str) -> CString {
    CString::new(s).unwrap()
}

#[test]
fn tcp_request_response_through_two_stacks() {
    // Real echo server the ingress side bridges to.
    let echo = TcpListener::bind("127.0.0.1:0").unwrap();
    let echo_addr = echo.local_addr().unwrap();
    std::thread::spawn(move || {
        for stream in echo.incoming() {
            let mut stream = match stream {
                Ok(s) => s,
                Err(_) => break,
            };
            std::thread::spawn(move || {
                let mut buf = [0u8; 4096];
                loop {
                    match stream.read(&mut buf) {
                        Ok(0) | Err(_) => break,
                        Ok(n) => {
                            // Echo back uppercased so we can tell it round-tripped.
                            let upper: Vec<u8> =
                                buf[..n].iter().map(|b| b.to_ascii_uppercase()).collect();
                            if stream.write_all(&upper).is_err() {
                                break;
                            }
                        }
                    }
                }
            });
        }
    });

    let wiring_a = Box::into_raw(Box::new(Wiring { peer: Mutex::new(None) }));
    let wiring_b = Box::into_raw(Box::new(Wiring { peer: Mutex::new(None) }));

    let stack_a = unsafe { ns_create(1420, forward_to_peer, wiring_a as *mut c_void) };
    let stack_b = unsafe { ns_create(1420, forward_to_peer, wiring_b as *mut c_void) };
    assert!(!stack_a.is_null());
    assert!(!stack_b.is_null());

    // Cross-wire: A emits -> B feeds, B emits -> A feeds.
    unsafe {
        *(*wiring_a).peer.lock().unwrap() = Some(stack_b);
        *(*wiring_b).peer.lock().unwrap() = Some(stack_a);
    }

    // Both planes on a shared /8 so each address is on-link to the other.
    let a_ip = cs("10.0.0.1/8");
    let b_ip = cs("10.0.0.2/8");
    unsafe {
        assert_eq!(ns_add_local_ip(stack_a, a_ip.as_ptr()), 0);
        assert_eq!(ns_add_local_ip(stack_b, b_ip.as_ptr()), 0);
    }

    // Ingress on B: virtual 10.0.0.2:80 -> real echo server.
    let target = cs(&format!("tcp:{echo_addr}"));
    let b_vip = cs("10.0.0.2");
    unsafe {
        assert_eq!(ns_add_tcp_forward(stack_b, b_vip.as_ptr(), 80, target.as_ptr()), 0);
    }

    // Egress on A: real loopback listener -> virtual connect to 10.0.0.2:80,
    // sourced from A's own virtual IP.
    let dst_ip = cs("10.0.0.2");
    let src_ip = cs("10.0.0.1");
    let loopback_port = unsafe { ns_add_tcp_egress(stack_a, dst_ip.as_ptr(), 80, src_ip.as_ptr()) };
    assert_ne!(loopback_port, 0, "egress listener failed to bind");

    // Drive a real TCP client through the whole path.
    std::thread::sleep(Duration::from_millis(100));
    let mut client = TcpStream::connect(("127.0.0.1", loopback_port)).unwrap();
    client.set_read_timeout(Some(Duration::from_secs(10))).unwrap();
    client.write_all(b"hello virtual world").unwrap();

    let mut response = Vec::new();
    let mut buf = [0u8; 1024];
    let deadline = std::time::Instant::now() + Duration::from_secs(10);
    while response.len() < b"hello virtual world".len() && std::time::Instant::now() < deadline {
        match client.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => response.extend_from_slice(&buf[..n]),
            Err(_) => break,
        }
    }

    assert_eq!(
        String::from_utf8_lossy(&response),
        "HELLO VIRTUAL WORLD",
        "data did not round-trip through both virtual stacks"
    );

    // Half-close propagation: dropping the client should tear down cleanly.
    drop(client);
    std::thread::sleep(Duration::from_millis(100));

    unsafe {
        ns_destroy(stack_a);
        ns_destroy(stack_b);
        drop(Box::from_raw(wiring_a));
        drop(Box::from_raw(wiring_b));
    }
}

/// A second, larger transfer to exercise multi-segment streaming and the
/// connection lifecycle a few times in a row.
#[test]
fn repeated_connections_and_larger_payload() {
    let echo = TcpListener::bind("127.0.0.1:0").unwrap();
    let echo_addr = echo.local_addr().unwrap();
    std::thread::spawn(move || {
        for stream in echo.incoming() {
            let mut stream = match stream { Ok(s) => s, Err(_) => break };
            std::thread::spawn(move || {
                let mut buf = [0u8; 8192];
                loop {
                    match stream.read(&mut buf) {
                        Ok(0) | Err(_) => break,
                        Ok(n) => { if stream.write_all(&buf[..n]).is_err() { break; } }
                    }
                }
            });
        }
    });

    let wiring_a = Box::into_raw(Box::new(Wiring { peer: Mutex::new(None) }));
    let wiring_b = Box::into_raw(Box::new(Wiring { peer: Mutex::new(None) }));
    let stack_a = unsafe { ns_create(1420, forward_to_peer, wiring_a as *mut c_void) };
    let stack_b = unsafe { ns_create(1420, forward_to_peer, wiring_b as *mut c_void) };
    unsafe {
        *(*wiring_a).peer.lock().unwrap() = Some(stack_b);
        *(*wiring_b).peer.lock().unwrap() = Some(stack_a);
        let a_ip = cs("10.0.0.1/8");
        let b_ip = cs("10.0.0.2/8");
        ns_add_local_ip(stack_a, a_ip.as_ptr());
        ns_add_local_ip(stack_b, b_ip.as_ptr());
        let target = cs(&format!("tcp:{echo_addr}"));
        let b_vip = cs("10.0.0.2");
        ns_add_tcp_forward(stack_b, b_vip.as_ptr(), 80, target.as_ptr());
    }
    let dst_ip = cs("10.0.0.2");
    let src_ip = cs("10.0.0.1");
    let port = unsafe { ns_add_tcp_egress(stack_a, dst_ip.as_ptr(), 80, src_ip.as_ptr()) };
    assert_ne!(port, 0);
    std::thread::sleep(Duration::from_millis(100));

    let payload = vec![b'x'; 200 * 1024]; // exceeds socket buffers -> multi-segment
    for _ in 0..3 {
        let mut client = TcpStream::connect(("127.0.0.1", port)).unwrap();
        client.set_read_timeout(Some(Duration::from_secs(15))).unwrap();
        let writer = {
            let payload = payload.clone();
            let mut c = client.try_clone().unwrap();
            std::thread::spawn(move || {
                c.write_all(&payload).unwrap();
                c.shutdown(std::net::Shutdown::Write).unwrap();
            })
        };
        let mut got = Vec::new();
        let mut buf = [0u8; 16384];
        loop {
            match client.read(&mut buf) {
                Ok(0) | Err(_) => break,
                Ok(n) => got.extend_from_slice(&buf[..n]),
            }
        }
        writer.join().unwrap();
        assert_eq!(got.len(), payload.len(), "echoed payload truncated");
    }

    unsafe {
        ns_destroy(stack_a);
        ns_destroy(stack_b);
        drop(Box::from_raw(wiring_a));
        drop(Box::from_raw(wiring_b));
    }
}

unsafe extern "C" fn discard_packets(_: *mut c_void, _: *const u8, _: usize) {}

#[test]
fn removing_egress_closes_only_its_loopback_listener() {
    let stack = unsafe { ns_create(1420, discard_packets, std::ptr::null_mut()) };
    assert!(!stack.is_null());
    let src = cs("10.0.0.1");
    let first = cs("10.0.0.2");
    let second = cs("10.0.0.3");
    let port_a = unsafe { ns_add_tcp_egress(stack, first.as_ptr(), 80, src.as_ptr()) };
    let port_b = unsafe { ns_add_tcp_egress(stack, second.as_ptr(), 80, src.as_ptr()) };
    assert!(port_a > 0 && port_b > 0);
    assert_eq!(unsafe { ns_remove_tcp_egress(stack, first.as_ptr(), 80) }, 0);
    let mut closed = false;
    for _ in 0..100 {
        if TcpStream::connect(("127.0.0.1", port_a)).is_err() { closed = true; break; }
        std::thread::sleep(Duration::from_millis(10));
    }
    assert!(closed);
    assert!(TcpStream::connect(("127.0.0.1", port_b)).is_ok());
    unsafe { ns_destroy(stack); }
}

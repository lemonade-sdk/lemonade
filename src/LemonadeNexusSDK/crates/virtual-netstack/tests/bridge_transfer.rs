//! Two stacks wired back to back (each one's output is the other's inbound),
//! with a loopback client on one side's egress and a loopback server behind
//! the other side's forward. Bytes must arrive whole and in order both ways,
//! including a half-close and a reader slower than the sender.

use std::ffi::{c_void, CString};
use std::io::{Read, Write};
use std::net::{Shutdown, TcpListener, TcpStream};
use std::sync::atomic::{AtomicPtr, Ordering};
use std::thread;
use std::time::{Duration, Instant};

use lemonade_virtual_netstack::*;

const MTU: u32 = 1420;
const TCP_BUFFER: u32 = 1 << 20;

unsafe extern "C" fn to_peer(ctx: *mut c_void, pkt: *const u8, len: usize) {
    let peer = (*(ctx as *const AtomicPtr<NsHandle>)).load(Ordering::Acquire);
    if !peer.is_null() {
        ns_feed_inbound(peer, pkt, len);
    }
}

struct Pair {
    a: *mut NsHandle,
    b: *mut NsHandle,
    a_out: Box<AtomicPtr<NsHandle>>,
    b_out: Box<AtomicPtr<NsHandle>>,
}

impl Pair {
    fn new() -> Pair {
        let a_out = Box::new(AtomicPtr::new(std::ptr::null_mut()));
        let b_out = Box::new(AtomicPtr::new(std::ptr::null_mut()));
        unsafe {
            let a = ns_create(MTU, TCP_BUFFER, to_peer, &*a_out as *const _ as *mut c_void);
            let b = ns_create(MTU, TCP_BUFFER, to_peer, &*b_out as *const _ as *mut c_void);
            assert!(!a.is_null() && !b.is_null());
            a_out.store(b, Ordering::Release);
            b_out.store(a, Ordering::Release);
            let ip = |s: &str| CString::new(s).unwrap();
            assert_eq!(ns_add_local_ip(a, ip("10.64.0.1/32").as_ptr()), 0);
            assert_eq!(ns_add_local_ip(b, ip("10.64.0.2/32").as_ptr()), 0);
            Pair { a, b, a_out, b_out }
        }
    }

    /// A loopback port on side A whose connections reach `server_port` behind side B.
    fn route(&self, vport: u16, server_port: u16) -> u16 {
        let s = |s: String| CString::new(s).unwrap();
        unsafe {
            assert_eq!(
                ns_add_tcp_forward(
                    self.b,
                    s("10.64.0.2".into()).as_ptr(),
                    vport,
                    s(format!("tcp:127.0.0.1:{server_port}")).as_ptr(),
                ),
                0
            );
            let port = ns_add_tcp_egress(
                self.a,
                s("10.64.0.2".into()).as_ptr(),
                vport,
                s("10.64.0.1".into()).as_ptr(),
            );
            assert_ne!(port, 0);
            port
        }
    }
}

impl Drop for Pair {
    fn drop(&mut self) {
        self.a_out.store(std::ptr::null_mut(), Ordering::Release);
        self.b_out.store(std::ptr::null_mut(), Ordering::Release);
        unsafe {
            ns_destroy(self.a);
            ns_destroy(self.b);
        }
    }
}

fn pattern(seed: u64, len: usize) -> Vec<u8> {
    let mut x = seed | 1;
    (0..len)
        .map(|_| {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            x as u8
        })
        .collect()
}

fn digest(data: &[u8]) -> u64 {
    data.iter().fold(0xcbf2_9ce4_8422_2325u64, |h, &b| {
        (h ^ b as u64).wrapping_mul(0x100_0000_01b3)
    })
}

/// The server reads the whole upload (until the client's FIN), optionally
/// slowly, then answers with its digest and `reply` bytes of pattern.
fn serve_once(listener: TcpListener, reply: usize, slow_reader: bool) -> thread::JoinHandle<()> {
    thread::spawn(move || {
        let (mut conn, _) = listener.accept().unwrap();
        let mut got = Vec::new();
        let mut buf = vec![0u8; 64 * 1024];
        let chunk = if slow_reader { 4096 } else { buf.len() };
        let mut reads = 0;
        loop {
            if slow_reader && reads < 40 {
                thread::sleep(Duration::from_millis(5));
            }
            let n = conn.read(&mut buf[..chunk]).unwrap();
            if n == 0 {
                break;
            }
            reads += 1;
            got.extend_from_slice(&buf[..n]);
        }
        conn.write_all(&digest(&got).to_le_bytes()).unwrap();
        conn.write_all(&(got.len() as u64).to_le_bytes()).unwrap();
        conn.write_all(&pattern(7, reply)).unwrap();
    })
}

fn transfer(upload: usize, reply: usize, slow_reader: bool) -> (Duration, Duration) {
    let pair = Pair::new();
    let listener = TcpListener::bind("127.0.0.1:0").unwrap();
    let server_port = listener.local_addr().unwrap().port();
    let server = serve_once(listener, reply, slow_reader);
    let port = pair.route(8080, server_port);

    let sent = pattern(3, upload);
    let mut client = TcpStream::connect(("127.0.0.1", port)).unwrap();
    client
        .set_read_timeout(Some(Duration::from_secs(60)))
        .unwrap();
    let started = Instant::now();
    client.write_all(&sent).unwrap();
    client.shutdown(Shutdown::Write).unwrap();

    let mut head = [0u8; 16];
    client.read_exact(&mut head).unwrap();
    let up = started.elapsed();
    assert_eq!(
        u64::from_le_bytes(head[8..].try_into().unwrap()),
        upload as u64,
        "upload length"
    );
    assert_eq!(
        u64::from_le_bytes(head[..8].try_into().unwrap()),
        digest(&sent),
        "upload bytes"
    );

    let down_started = Instant::now();
    let mut back = Vec::new();
    client.read_to_end(&mut back).unwrap();
    let down = down_started.elapsed();
    assert_eq!(back.len(), reply, "reply length");
    assert!(back == pattern(7, reply), "reply bytes");
    server.join().unwrap();
    (up, down)
}

#[test]
fn small_request_and_reply_arrive_whole() {
    transfer(300, 2000, false);
}

#[test]
fn bulk_both_ways_arrives_whole_and_in_order() {
    let size = 32 << 20;
    let (up, down) = transfer(size, size, false);
    let mbps = |d: Duration| size as f64 / d.as_secs_f64() / 1e6;
    eprintln!(
        "bridge transfer, 32 MiB: up {:.0} MB/s, down {:.0} MB/s",
        mbps(up),
        mbps(down)
    );
}

#[test]
fn slow_reader_backpressure_loses_nothing() {
    transfer(8 << 20, 1 << 20, true);
}

//! C FFI for the in-process virtual netstack.
//!
//! The boringtun dataplane (C++) owns the UDP socket and per-peer Noise
//! sessions; this crate terminates the TCP/UDP that is addressed to the
//! server's own virtual IPs, with no kernel network interface. Decrypted IP
//! packets are fed in with `ns_feed_inbound`; packets the stack emits are
//! handed back through the `ns_output_fn` callback for re-encryption.

mod bridge;
mod device;
mod stack;

use std::ffi::{c_void, CStr};
use std::net::{Ipv4Addr, SocketAddr, SocketAddrV4, TcpListener as StdTcpListener};
use std::os::raw::{c_char, c_int};
use std::rc::Rc;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::mpsc::{channel, Sender};
use std::sync::Arc;
use std::thread::JoinHandle;

use mio::{Token, Waker};
use smoltcp::wire::{IpCidr, IpEndpoint, Ipv4Address};

use bridge::BridgeTarget;
use stack::{Command, Stack};

/// Callback invoked with each outbound IP packet the stack emits.
pub type NsOutputFn = unsafe extern "C" fn(ctx: *mut c_void, pkt: *const u8, len: usize);

/// Wrapper making the C callback + context movable into the stack thread.
struct OutputCallback {
    f: NsOutputFn,
    ctx: *mut c_void,
}
// Safety: the C++ owner guarantees `ctx` outlives the stack and that `f` is
// safe to call from the stack thread.
unsafe impl Send for OutputCallback {}

impl OutputCallback {
    fn emit(&self, pkt: &[u8]) {
        unsafe { (self.f)(self.ctx, pkt.as_ptr(), pkt.len()) }
    }
}

pub struct NsHandle {
    cmd_tx: Sender<Command>,
    pkt_tx: Sender<Vec<u8>>,
    waker: Arc<Waker>,
    egress_token: AtomicU32,
    join: Option<JoinHandle<()>>,
}

const EGRESS_TOKEN_BASE: u32 = 0x4000_0000;

unsafe fn cstr<'a>(p: *const c_char) -> Option<&'a str> {
    if p.is_null() {
        return None;
    }
    CStr::from_ptr(p).to_str().ok()
}

fn parse_ipv4(s: &str) -> Option<Ipv4Address> {
    s.parse::<Ipv4Addr>().ok().map(|a| Ipv4Address::from(a))
}

/// Parse "ip/prefix" (or a bare address, treated as /32) into an IpCidr.
fn parse_cidr(s: &str) -> Option<IpCidr> {
    let (ip, prefix) = match s.split_once('/') {
        Some((ip, p)) => (ip, p.parse::<u8>().ok()?),
        None => (s, 32),
    };
    let addr = parse_ipv4(ip)?;
    if prefix > 32 {
        return None;
    }
    Some(IpCidr::new(addr.into(), prefix))
}

/// Create a netstack. Returns null on failure.
///
/// # Safety
/// `output` must be a valid function pointer; `ctx` must outlive the stack.
#[no_mangle]
pub unsafe extern "C" fn ns_create(
    mtu: u32,
    output: NsOutputFn,
    ctx: *mut c_void,
) -> *mut NsHandle {
    let callback = OutputCallback { f: output, ctx };
    let (cmd_tx, cmd_rx) = channel::<Command>();
    let (pkt_tx, pkt_rx) = channel::<Vec<u8>>();

    let mtu = mtu as usize;
    let (waker_tx, waker_rx) = channel::<Arc<Waker>>();

    let join = std::thread::Builder::new()
        .name("virtual-netstack".into())
        .spawn(move || {
            let output_rc: Rc<device::OutputFn> = Rc::new(move |pkt: &[u8]| callback.emit(pkt));
            let (stack, waker) = match Stack::new(mtu, output_rc, cmd_rx, pkt_rx) {
                Ok(v) => v,
                Err(e) => {
                    log::error!("virtual-netstack: failed to start: {e}");
                    let _ = waker_tx.send(Arc::new(
                        // Unreachable use; channel just needs a value on error.
                        Waker::new(&mio::Poll::new().unwrap().registry().try_clone().unwrap(), Token(0))
                            .unwrap(),
                    ));
                    return;
                }
            };
            let _ = waker_tx.send(Arc::new(waker));
            stack.run();
        });

    let join = match join {
        Ok(j) => j,
        Err(_) => return std::ptr::null_mut(),
    };

    let waker = match waker_rx.recv() {
        Ok(w) => w,
        Err(_) => return std::ptr::null_mut(),
    };

    Box::into_raw(Box::new(NsHandle {
        cmd_tx,
        pkt_tx,
        waker,
        egress_token: AtomicU32::new(EGRESS_TOKEN_BASE),
        join: Some(join),
    }))
}

/// Stop the stack thread and free the handle.
///
/// # Safety
/// `handle` must be a pointer returned by `ns_create` (or null).
#[no_mangle]
pub unsafe extern "C" fn ns_destroy(handle: *mut NsHandle) {
    if handle.is_null() {
        return;
    }
    let mut handle = Box::from_raw(handle);
    let _ = handle.cmd_tx.send(Command::Shutdown);
    let _ = handle.waker.wake();
    if let Some(join) = handle.join.take() {
        let _ = join.join();
    }
}

/// Register a virtual local address the stack answers for, e.g. "10.64.0.1/10".
///
/// # Safety
/// `handle` valid; `cidr` a NUL-terminated UTF-8 string.
#[no_mangle]
pub unsafe extern "C" fn ns_add_local_ip(handle: *mut NsHandle, cidr: *const c_char) -> c_int {
    let handle = match handle.as_ref() {
        Some(h) => h,
        None => return -1,
    };
    let cidr = match cstr(cidr).and_then(parse_cidr) {
        Some(c) => c,
        None => return -1,
    };
    if handle.cmd_tx.send(Command::AddLocalIp(cidr)).is_err() {
        return -1;
    }
    let _ = handle.waker.wake();
    0
}

/// Feed a decrypted inbound IP packet into the stack. Thread-safe.
///
/// # Safety
/// `handle` valid; `pkt` points to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn ns_feed_inbound(handle: *mut NsHandle, pkt: *const u8, len: usize) {
    let handle = match handle.as_ref() {
        Some(h) => h,
        None => return,
    };
    if pkt.is_null() || len == 0 {
        return;
    }
    let data = std::slice::from_raw_parts(pkt, len).to_vec();
    if handle.pkt_tx.send(data).is_ok() {
        let _ = handle.waker.wake();
    }
}

/// Add an ingress forward: virtual `vip:vport` -> bridge `target`.
/// `target` is "tcp:127.0.0.1:PORT" or "unix:/path".
///
/// # Safety
/// `handle` valid; string args NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn ns_add_tcp_forward(
    handle: *mut NsHandle,
    vip: *const c_char,
    vport: u16,
    target: *const c_char,
) -> c_int {
    let handle = match handle.as_ref() {
        Some(h) => h,
        None => return -1,
    };
    let vip = match cstr(vip).and_then(parse_ipv4) {
        Some(v) => v,
        None => return -1,
    };
    let target = match cstr(target).and_then(BridgeTarget::parse) {
        Some(t) => t,
        None => return -1,
    };
    if handle
        .cmd_tx
        .send(Command::AddTcpForward { vip, vport, target })
        .is_err()
    {
        return -1;
    }
    let _ = handle.waker.wake();
    0
}

/// Add an egress: bind a real loopback TCP listener and bridge each accepted
/// connection to a virtual TCP connect toward `dst_ip:dst_port`, sourced from
/// `src_ip` (one of our virtual addresses). Returns the bound loopback port,
/// or 0 on failure.
///
/// # Safety
/// `handle` valid; string args NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn ns_add_tcp_egress(
    handle: *mut NsHandle,
    dst_ip: *const c_char,
    dst_port: u16,
    src_ip: *const c_char,
) -> u16 {
    let handle = match handle.as_ref() {
        Some(h) => h,
        None => return 0,
    };
    let dst_ip = match cstr(dst_ip).and_then(parse_ipv4) {
        Some(v) => v,
        None => return 0,
    };
    let src_ip = match cstr(src_ip).and_then(parse_ipv4) {
        Some(v) => v,
        None => return 0,
    };

    let std_listener = match StdTcpListener::bind(SocketAddr::V4(SocketAddrV4::new(
        Ipv4Addr::LOCALHOST,
        0,
    ))) {
        Ok(l) => l,
        Err(_) => return 0,
    };
    let bound_port = match std_listener.local_addr() {
        Ok(SocketAddr::V4(a)) => a.port(),
        _ => return 0,
    };
    if std_listener.set_nonblocking(true).is_err() {
        return 0;
    }
    let listener = mio::net::TcpListener::from_std(std_listener);
    let token = Token(handle.egress_token.fetch_add(1, Ordering::Relaxed) as usize);
    let dst = IpEndpoint {
        addr: dst_ip.into(),
        port: dst_port,
    };

    if handle
        .cmd_tx
        .send(Command::AddTcpEgress {
            listener,
            token,
            dst,
            src_ip,
        })
        .is_err()
    {
        return 0;
    }
    let _ = handle.waker.wake();
    bound_port
}

#[no_mangle]
pub unsafe extern "C" fn ns_remove_tcp_egress(handle: *mut NsHandle, dst_ip: *const c_char, dst_port: u16) -> c_int {
    let handle = match handle.as_ref() { Some(h) => h, None => return -1 };
    let addr = match cstr(dst_ip).and_then(parse_ipv4) { Some(ip) => ip, None => return -1 };
    let dst = IpEndpoint { addr: addr.into(), port: dst_port };
    if handle.cmd_tx.send(Command::RemoveTcpEgress(dst)).is_err() { return -1; }
    let _ = handle.waker.wake();
    0
}

//! The in-process netstack run loop. Owns the smoltcp interface and bridges
//! virtual TCP connections to/from real local sockets, driven by a single
//! dedicated thread and a mio event loop.

use std::collections::HashMap;
use std::io;
use std::rc::Rc;
use std::sync::mpsc::{Receiver, TryRecvError};
use std::time::{Duration, Instant as StdInstant};

use mio::event::Source;
use mio::net::TcpListener;
use mio::{Events, Interest, Poll, Token, Waker};

use smoltcp::iface::{Config, Interface, SocketHandle, SocketSet};
use smoltcp::socket::tcp;
use smoltcp::time::Instant;
use smoltcp::wire::{IpAddress, IpCidr, IpEndpoint, IpListenEndpoint, Ipv4Address};

use crate::bridge::{Bridge, BridgeTarget};
use crate::device::VirtDevice;

const WAKER_TOKEN: Token = Token(0);
const SOCK_BUF: usize = 65536;
const MAX_INNER_ITERS: usize = 32;

/// Commands sent from FFI callers to the stack thread.
pub enum Command {
    AddLocalIp(IpCidr),
    AddTcpForward {
        vip: Ipv4Address,
        vport: u16,
        target: BridgeTarget,
    },
    /// Real loopback listener -> virtual TCP connect to (dst_ip, dst_port).
    AddTcpEgress {
        listener: TcpListener,
        token: Token,
        dst: IpEndpoint,
        src_ip: Ipv4Address,
    },
    RemoveTcpEgress(IpEndpoint),
    Shutdown,
}

struct Listener {
    endpoint: IpListenEndpoint,
    target_spec: String,
    handle: SocketHandle,
}

struct EgressListener {
    listener: TcpListener,
    dst: IpEndpoint,
    src_ip: Ipv4Address,
}

struct Conn {
    tcp: SocketHandle,
    bridge: Bridge,
    to_bridge: Vec<u8>,   // decrypted-side bytes waiting to flush to the bridge
    to_tcp: Vec<u8>,      // bridge-side bytes waiting to flush into smoltcp
    established: bool,     // virtual socket reached Established at least once
    bridge_eof: bool,     // bridge read returned 0
    fin_to_bridge: bool,  // we already half-closed the bridge write side
}

pub struct Stack {
    iface: Interface,
    device: VirtDevice,
    sockets: SocketSet<'static>,
    poll: Poll,
    events: Events,
    listeners: Vec<Listener>,
    egress: HashMap<Token, EgressListener>,
    conns: HashMap<Token, Conn>,
    next_token: usize,
    next_ephemeral: u16,
    base: StdInstant,
    cmd_rx: Receiver<Command>,
    pkt_rx: Receiver<Vec<u8>>,
}

impl Stack {
    pub fn new(
        mtu: usize,
        output: Rc<crate::device::OutputFn>,
        cmd_rx: Receiver<Command>,
        pkt_rx: Receiver<Vec<u8>>,
    ) -> io::Result<(Stack, Waker)> {
        let mut device = VirtDevice::new(mtu, output);
        let config = Config::new(smoltcp::wire::HardwareAddress::Ip);
        let now = Instant::from_millis(0);
        let iface = Interface::new(config, &mut device, now);

        let poll = Poll::new()?;
        let waker = Waker::new(poll.registry(), WAKER_TOKEN)?;

        Ok((
            Stack {
                iface,
                device,
                sockets: SocketSet::new(Vec::new()),
                poll,
                events: Events::with_capacity(256),
                listeners: Vec::new(),
                egress: HashMap::new(),
                conns: HashMap::new(),
                next_token: 1,
                next_ephemeral: 49152,
                base: StdInstant::now(),
                cmd_rx,
                pkt_rx,
            },
            waker,
        ))
    }

    fn now(&self) -> Instant {
        Instant::from_millis(self.base.elapsed().as_millis() as i64)
    }

    fn alloc_token(&mut self) -> Token {
        let t = Token(self.next_token);
        self.next_token += 1;
        t
    }

    fn alloc_ephemeral(&mut self) -> u16 {
        let p = self.next_ephemeral;
        self.next_ephemeral = if self.next_ephemeral >= 65535 {
            49152
        } else {
            self.next_ephemeral + 1
        };
        p
    }

    fn make_tcp_socket() -> tcp::Socket<'static> {
        let rx = tcp::SocketBuffer::new(vec![0u8; SOCK_BUF]);
        let tx = tcp::SocketBuffer::new(vec![0u8; SOCK_BUF]);
        tcp::Socket::new(rx, tx)
    }

    fn new_listening_socket(&mut self, endpoint: IpListenEndpoint) -> Option<SocketHandle> {
        let mut socket = Self::make_tcp_socket();
        if socket.listen(endpoint).is_err() {
            log::error!("virtual-netstack: failed to listen on {:?}", endpoint);
            return None;
        }
        Some(self.sockets.add(socket))
    }

    fn handle_command(&mut self, cmd: Command) -> bool {
        match cmd {
            Command::AddLocalIp(cidr) => {
                self.iface.update_ip_addrs(|addrs| {
                    if !addrs.iter().any(|a| *a == cidr) {
                        let _ = addrs.push(cidr);
                    }
                });
                log::info!("virtual-netstack: now answering for {}", cidr);
            }
            Command::AddTcpForward {
                vip,
                vport,
                target,
            } => {
                let endpoint = IpListenEndpoint {
                    addr: Some(IpAddress::Ipv4(vip)),
                    port: vport,
                };
                if let Some(handle) = self.new_listening_socket(endpoint) {
                    let spec = match &target {
                        BridgeTarget::Tcp(a) => format!("tcp:{a}"),
                        BridgeTarget::Unix(p) => format!("unix:{p}"),
                    };
                    self.listeners.push(Listener {
                        endpoint,
                        target_spec: spec,
                        handle,
                    });
                    log::info!("virtual-netstack: ingress {vip}:{vport} -> bridge");
                }
            }
            Command::AddTcpEgress {
                mut listener,
                token,
                dst,
                src_ip,
            } => {
                if listener
                    .register(self.poll.registry(), token, Interest::READABLE)
                    .is_ok()
                {
                    self.egress.insert(
                        token,
                        EgressListener {
                            listener,
                            dst,
                            src_ip,
                        },
                    );
                }
            }
            Command::RemoveTcpEgress(dst) => {
                let listeners: Vec<Token> = self.egress.iter()
                    .filter_map(|(token, listener)| (listener.dst == dst).then_some(*token)).collect();
                for token in listeners {
                    if let Some(mut listener) = self.egress.remove(&token) {
                        let _ = listener.listener.deregister(self.poll.registry());
                    }
                }
                let connections: Vec<Token> = self.conns.iter().filter_map(|(token, conn)| {
                    (self.sockets.get::<tcp::Socket>(conn.tcp).remote_endpoint() == Some(dst)).then_some(*token)
                }).collect();
                for token in connections {
                    if let Some(mut conn) = self.conns.remove(&token) {
                        self.sockets.get_mut::<tcp::Socket>(conn.tcp).abort();
                        conn.bridge.deregister(self.poll.registry());
                        self.sockets.remove(conn.tcp);
                    }
                }
            }
            Command::Shutdown => return false,
        }
        true
    }

    /// Promote any listener whose socket accepted a connection, then bring up
    /// the real bridge for it.
    fn manage_listeners(&mut self) {
        let mut promoted: Vec<(usize, SocketHandle, String, IpListenEndpoint)> = Vec::new();
        for (idx, listener) in self.listeners.iter().enumerate() {
            let socket = self.sockets.get::<tcp::Socket>(listener.handle);
            if socket.state() != tcp::State::Listen {
                promoted.push((
                    idx,
                    listener.handle,
                    listener.target_spec.clone(),
                    listener.endpoint,
                ));
            }
        }

        for (idx, handle, spec, endpoint) in promoted {
            // Replace the listening socket so the next connection is accepted.
            if let Some(new_handle) = self.new_listening_socket(endpoint) {
                self.listeners[idx].handle = new_handle;
            }

            let target = match BridgeTarget::parse(&spec) {
                Some(t) => t,
                None => {
                    self.abort_socket(handle);
                    continue;
                }
            };
            match target.connect() {
                Ok(mut bridge) => {
                    let token = self.alloc_token();
                    if bridge.register(self.poll.registry(), token).is_err() {
                        self.abort_socket(handle);
                        continue;
                    }
                    self.conns.insert(
                        token,
                        Conn {
                            tcp: handle,
                            bridge,
                            to_bridge: Vec::new(),
                            to_tcp: Vec::new(),
                            established: false,
                            bridge_eof: false,
                            fin_to_bridge: false,
                        },
                    );
                }
                Err(e) => {
                    log::warn!("virtual-netstack: bridge connect failed: {e}");
                    self.abort_socket(handle);
                }
            }
        }
    }

    /// Accept real loopback connections on egress listeners and open the paired
    /// virtual TCP connection.
    fn manage_egress(&mut self, ready: &[Token]) {
        let tokens: Vec<Token> = ready
            .iter()
            .copied()
            .filter(|t| self.egress.contains_key(t))
            .collect();
        for token in tokens {
            loop {
                let accepted = {
                    let eg = self.egress.get(&token).unwrap();
                    eg.listener.accept()
                };
                let (stream, dst, src_ip) = match accepted {
                    Ok((stream, _)) => {
                        let eg = self.egress.get(&token).unwrap();
                        (stream, eg.dst, eg.src_ip)
                    }
                    Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => break,
                    Err(_) => break,
                };

                let mut socket = Self::make_tcp_socket();
                let local_port = self.alloc_ephemeral();
                let local = IpEndpoint {
                    addr: IpAddress::Ipv4(src_ip),
                    port: local_port,
                };
                let cx = self.iface.context();
                if socket.connect(cx, dst, local).is_err() {
                    continue;
                }
                let handle = self.sockets.add(socket);

                let mut bridge = Bridge::Tcp(stream);
                let conn_token = self.alloc_token();
                if bridge.register(self.poll.registry(), conn_token).is_err() {
                    self.abort_socket(handle);
                    continue;
                }
                self.conns.insert(
                    conn_token,
                    Conn {
                        tcp: handle,
                        bridge,
                        to_bridge: Vec::new(),
                        to_tcp: Vec::new(),
                        established: false,
                        bridge_eof: false,
                        fin_to_bridge: false,
                    },
                );
            }
        }
    }

    fn abort_socket(&mut self, handle: SocketHandle) {
        self.sockets.get_mut::<tcp::Socket>(handle).abort();
        self.sockets.remove(handle);
    }

    /// Move bytes between every connection's smoltcp socket and its bridge.
    /// Returns true if any byte moved (so the caller re-polls smoltcp).
    fn pump_conns(&mut self) -> bool {
        let mut progressed = false;
        let mut finished: Vec<Token> = Vec::new();
        let tokens: Vec<Token> = self.conns.keys().copied().collect();
        let mut buf = [0u8; 16384];

        for token in tokens {
            let mut remove = false;
            {
                let conn = self.conns.get_mut(&token).unwrap();
                let socket = self.sockets.get_mut::<tcp::Socket>(conn.tcp);

                if socket.may_send() {
                    conn.established = true;
                }

                // smoltcp -> bridge
                while socket.can_recv() {
                    match socket.recv_slice(&mut buf) {
                        Ok(0) => break,
                        Ok(n) => {
                            conn.to_bridge.extend_from_slice(&buf[..n]);
                            progressed = true;
                        }
                        Err(_) => break,
                    }
                }
                if flush(&mut conn.bridge, &mut conn.to_bridge) {
                    progressed = true;
                }
                // Virtual peer half-closed and we've drained: FIN the bridge.
                // Only meaningful once established — !may_recv() is also true in
                // the pre-established SynReceived/SynSent states.
                if conn.established
                    && !conn.fin_to_bridge
                    && conn.to_bridge.is_empty()
                    && !socket.may_recv()
                    && socket.recv_queue() == 0
                {
                    conn.bridge.shutdown_write();
                    conn.fin_to_bridge = true;
                }

                // bridge -> smoltcp
                if !conn.bridge_eof {
                    loop {
                        match conn.bridge.read(&mut buf) {
                            Ok(0) => {
                                conn.bridge_eof = true;
                                break;
                            }
                            Ok(n) => {
                                conn.to_tcp.extend_from_slice(&buf[..n]);
                                progressed = true;
                            }
                            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => break,
                            Err(_) => {
                                conn.bridge_eof = true;
                                break;
                            }
                        }
                    }
                }
                if socket.can_send() && !conn.to_tcp.is_empty() {
                    if let Ok(n) = socket.send_slice(&conn.to_tcp) {
                        if n > 0 {
                            conn.to_tcp.drain(..n);
                            progressed = true;
                        }
                    }
                }
                // Bridge fully drained to the virtual peer: close the virtual side.
                if conn.established && conn.bridge_eof && conn.to_tcp.is_empty() && socket.may_send() {
                    socket.close();
                }

                let tcp_closed = matches!(socket.state(), tcp::State::Closed);
                if tcp_closed && conn.to_bridge.is_empty() {
                    remove = true;
                }
            }
            if remove {
                finished.push(token);
            }
        }

        for token in finished {
            if let Some(mut conn) = self.conns.remove(&token) {
                conn.bridge.deregister(self.poll.registry());
                self.sockets.remove(conn.tcp);
            }
        }
        progressed
    }

    pub fn run(mut self) {
        let mut running = true;
        while running {
            // Drain control commands.
            loop {
                match self.cmd_rx.try_recv() {
                    Ok(cmd) => {
                        if !self.handle_command(cmd) {
                            running = false;
                        }
                    }
                    Err(TryRecvError::Empty) => break,
                    Err(TryRecvError::Disconnected) => {
                        running = false;
                        break;
                    }
                }
            }
            if !running {
                break;
            }

            // Drain inbound IP packets fed by the dataplane.
            loop {
                match self.pkt_rx.try_recv() {
                    Ok(pkt) => self.device.push_inbound(pkt),
                    Err(_) => break,
                }
            }

            // Drive smoltcp and the bridges until no further immediate progress.
            for _ in 0..MAX_INNER_ITERS {
                let now = self.now();
                let polled_changed = self.iface.poll(now, &mut self.device, &mut self.sockets);
                self.manage_listeners();
                let pumped = self.pump_conns();
                if !polled_changed && !pumped && !self.device.has_inbound() {
                    break;
                }
            }

            // Sleep until the next smoltcp timer, an inbound packet, a control
            // command (both via the waker), or bridge-socket readiness.
            let timeout = self
                .iface
                .poll_delay(self.now(), &self.sockets)
                .map(|d| Duration::from_micros(d.total_micros()))
                .unwrap_or(Duration::from_millis(1000));
            let timeout = timeout.min(Duration::from_millis(1000));

            self.events.clear();
            if let Err(e) = self.poll.poll(&mut self.events, Some(timeout)) {
                if e.kind() != io::ErrorKind::Interrupted {
                    log::error!("virtual-netstack: poll error: {e}");
                }
            }
            let ready: Vec<Token> = self.events.iter().map(|e| e.token()).collect();
            self.manage_egress(&ready);
        }

        // Teardown: deregister bridges.
        for (_, mut conn) in self.conns.drain() {
            conn.bridge.deregister(self.poll.registry());
        }
        for (_, mut eg) in self.egress.drain() {
            let _ = eg.listener.deregister(self.poll.registry());
        }
        log::info!("virtual-netstack: stopped");
    }
}

/// Flush as much of `pending` to the bridge as possible. Returns true if any
/// bytes were written.
fn flush(bridge: &mut Bridge, pending: &mut Vec<u8>) -> bool {
    let mut wrote = 0;
    while !pending.is_empty() {
        match bridge.write(&pending[wrote..]) {
            Ok(0) => break,
            Ok(n) => wrote += n,
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => break,
            Err(_) => break,
        }
        if wrote == pending.len() {
            break;
        }
    }
    if wrote > 0 {
        pending.drain(..wrote);
        true
    } else {
        false
    }
}

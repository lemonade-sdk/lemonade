//! A bridge endpoint: the real local socket that a virtual TCP connection is
//! piped to (the unchanged httplib private API server). Either a loopback TCP
//! socket or a Unix-domain socket. Non-blocking, driven by the stack's mio
//! event loop.

use std::io::{self, Read, Write};
use std::net::SocketAddr;
#[cfg(unix)]
use std::path::Path;

use mio::event::Source;
use mio::{Interest, Registry, Token};

pub enum BridgeTarget {
    Tcp(SocketAddr),
    Unix(String),
}

impl BridgeTarget {
    /// Parse a target spec: "tcp:127.0.0.1:9101" or "unix:/path" (a leading '@'
    /// in the path selects the Linux abstract namespace).
    pub fn parse(spec: &str) -> Option<BridgeTarget> {
        if let Some(rest) = spec.strip_prefix("tcp:") {
            rest.parse().ok().map(BridgeTarget::Tcp)
        } else if let Some(rest) = spec.strip_prefix("unix:") {
            Some(BridgeTarget::Unix(rest.to_string()))
        } else {
            None
        }
    }

    pub fn connect(&self) -> io::Result<Bridge> {
        match self {
            BridgeTarget::Tcp(addr) => Ok(Bridge::Tcp(mio::net::TcpStream::connect(*addr)?)),
            #[cfg(unix)]
            BridgeTarget::Unix(path) => {
                Ok(Bridge::Unix(mio::net::UnixStream::connect(Path::new(path))?))
            }
            #[cfg(not(unix))]
            BridgeTarget::Unix(_) => Err(io::Error::new(
                io::ErrorKind::Unsupported,
                "unix-socket bridge unsupported on this platform",
            )),
        }
    }
}

pub enum Bridge {
    Tcp(mio::net::TcpStream),
    #[cfg(unix)]
    Unix(mio::net::UnixStream),
}

impl Bridge {
    pub fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        match self {
            Bridge::Tcp(s) => s.read(buf),
            #[cfg(unix)]
            Bridge::Unix(s) => s.read(buf),
        }
    }

    pub fn write(&mut self, buf: &[u8]) -> io::Result<usize> {
        match self {
            Bridge::Tcp(s) => s.write(buf),
            #[cfg(unix)]
            Bridge::Unix(s) => s.write(buf),
        }
    }

    /// Shut down the write half (propagates a virtual-side FIN to the bridge).
    pub fn shutdown_write(&mut self) {
        match self {
            Bridge::Tcp(s) => {
                let _ = s.shutdown(std::net::Shutdown::Write);
            }
            #[cfg(unix)]
            Bridge::Unix(s) => {
                let _ = s.shutdown(std::net::Shutdown::Write);
            }
        }
    }

    pub fn register(&mut self, registry: &Registry, token: Token) -> io::Result<()> {
        let interest = Interest::READABLE | Interest::WRITABLE;
        match self {
            Bridge::Tcp(s) => s.register(registry, token, interest),
            #[cfg(unix)]
            Bridge::Unix(s) => s.register(registry, token, interest),
        }
    }

    pub fn deregister(&mut self, registry: &Registry) {
        match self {
            Bridge::Tcp(s) => {
                let _ = s.deregister(registry);
            }
            #[cfg(unix)]
            Bridge::Unix(s) => {
                let _ = s.deregister(registry);
            }
        }
    }
}

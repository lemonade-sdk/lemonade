//! A smoltcp `Device` with no hardware behind it: received IP packets are fed
//! in from the boringtun dataplane, and transmitted IP packets are handed to a
//! C callback (`send_outbound_ip_packet`). Ethernet/ARP are absent — this is a
//! point-to-point IP medium.

use std::collections::VecDeque;
use std::rc::Rc;

use smoltcp::phy::{Device, DeviceCapabilities, Medium};
use smoltcp::time::Instant;

/// Sink for outbound IP packets (forwarded to the C dataplane).
pub type OutputFn = dyn Fn(&[u8]);

pub struct VirtDevice {
    rx_queue: VecDeque<Vec<u8>>,
    mtu: usize,
    output: Rc<OutputFn>,
}

impl VirtDevice {
    pub fn new(mtu: usize, output: Rc<OutputFn>) -> Self {
        Self {
            rx_queue: VecDeque::new(),
            mtu,
            output,
        }
    }

    /// Enqueue an inbound IP packet to be processed on the next poll.
    pub fn push_inbound(&mut self, packet: Vec<u8>) {
        self.rx_queue.push_back(packet);
    }

    pub fn has_inbound(&self) -> bool {
        !self.rx_queue.is_empty()
    }
}

pub struct VirtRxToken(Vec<u8>);

pub struct VirtTxToken {
    output: Rc<OutputFn>,
}

impl smoltcp::phy::RxToken for VirtRxToken {
    fn consume<R, F>(mut self, f: F) -> R
    where
        F: FnOnce(&mut [u8]) -> R,
    {
        f(&mut self.0)
    }
}

impl smoltcp::phy::TxToken for VirtTxToken {
    fn consume<R, F>(self, len: usize, f: F) -> R
    where
        F: FnOnce(&mut [u8]) -> R,
    {
        let mut buffer = vec![0u8; len];
        let result = f(&mut buffer);
        (self.output)(&buffer);
        result
    }
}

impl Device for VirtDevice {
    type RxToken<'a> = VirtRxToken;
    type TxToken<'a> = VirtTxToken;

    fn receive(&mut self, _timestamp: Instant) -> Option<(Self::RxToken<'_>, Self::TxToken<'_>)> {
        let packet = self.rx_queue.pop_front()?;
        Some((
            VirtRxToken(packet),
            VirtTxToken {
                output: self.output.clone(),
            },
        ))
    }

    fn transmit(&mut self, _timestamp: Instant) -> Option<Self::TxToken<'_>> {
        Some(VirtTxToken {
            output: self.output.clone(),
        })
    }

    fn capabilities(&self) -> DeviceCapabilities {
        let mut caps = DeviceCapabilities::default();
        caps.medium = Medium::Ip;
        caps.max_transmission_unit = self.mtu;
        caps
    }
}

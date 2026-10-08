// Re-export BoringTun's C FFI symbols so they end up in our static library.
pub use boringtun::ffi::*;

use boringtun::noise::handshake::parse_handshake_anon;
use boringtun::noise::{Packet, Tunn};
use boringtun::x25519::{PublicKey, StaticSecret};
use std::ffi::CStr;
use std::os::raw::c_char;

unsafe fn decode_key_b64(s: *const c_char) -> Option<[u8; 32]> {
    if s.is_null() {
        return None;
    }
    let s = CStr::from_ptr(s).to_str().ok()?;
    let bytes = base64::decode(s).ok()?;
    bytes.try_into().ok()
}

/// Identify the initiator of a WireGuard handshake-initiation packet without
/// touching any per-peer Tunn state. Performs one DH against our static key
/// and decrypts the initiator's static public key.
///
/// Returns 0 on success and writes the initiator's raw X25519 public key
/// (32 bytes) to `out_peer_static_public`. Returns -1 if the packet is not a
/// well-formed handshake initiation or authentication fails.
///
/// # Safety
/// `src` must point to `src_size` readable bytes; `out_peer_static_public`
/// must point to 32 writable bytes; key strings must be NUL-terminated base64.
#[no_mangle]
pub unsafe extern "C" fn wireguard_parse_handshake_anon(
    server_static_private: *const c_char,
    server_static_public: *const c_char,
    src: *const u8,
    src_size: u32,
    out_peer_static_public: *mut u8,
) -> i32 {
    if src.is_null() || out_peer_static_public.is_null() {
        return -1;
    }
    let (private_key, public_key) = match (
        decode_key_b64(server_static_private),
        decode_key_b64(server_static_public),
    ) {
        (Some(sk), Some(pk)) => (StaticSecret::from(sk), PublicKey::from(pk)),
        _ => return -1,
    };

    let packet = std::slice::from_raw_parts(src, src_size as usize);
    let init = match Tunn::parse_incoming_packet(packet) {
        Ok(Packet::HandshakeInit(init)) => init,
        _ => return -1,
    };
    match parse_handshake_anon(&private_key, &public_key, &init) {
        Ok(half) => {
            std::ptr::copy_nonoverlapping(
                half.peer_static_public.as_ptr(),
                out_peer_static_public,
                32,
            );
            0
        }
        Err(_) => -1,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CString;

    fn b64_keypair() -> (CString, CString) {
        let sk = StaticSecret::random_from_rng(rand_core::OsRng);
        let pk = PublicKey::from(&sk);
        (
            CString::new(base64::encode(sk.to_bytes())).unwrap(),
            CString::new(base64::encode(pk.as_bytes())).unwrap(),
        )
    }

    #[test]
    fn parse_handshake_anon_identifies_initiator() {
        let (server_sk, server_pk) = b64_keypair();
        let (client_sk, client_pk) = b64_keypair();

        let tunn = unsafe {
            new_tunnel(
                client_sk.as_ptr(),
                server_pk.as_ptr(),
                std::ptr::null(),
                25,
                7,
            )
        };
        assert!(!tunn.is_null());

        let mut init = [0u8; 256];
        let res = unsafe { wireguard_force_handshake(tunn, init.as_mut_ptr(), init.len() as u32) };
        assert!(matches!(res.op, result_type::WRITE_TO_NETWORK));

        let mut peer_pub = [0u8; 32];
        let rc = unsafe {
            wireguard_parse_handshake_anon(
                server_sk.as_ptr(),
                server_pk.as_ptr(),
                init.as_ptr(),
                res.size as u32,
                peer_pub.as_mut_ptr(),
            )
        };
        assert_eq!(rc, 0);
        let expected = base64::decode(client_pk.to_str().unwrap()).unwrap();
        assert_eq!(peer_pub.as_slice(), expected.as_slice());

        // Garbage input must be rejected.
        let rc = unsafe {
            wireguard_parse_handshake_anon(
                server_sk.as_ptr(),
                server_pk.as_ptr(),
                init.as_ptr(),
                10,
                peer_pub.as_mut_ptr(),
            )
        };
        assert_eq!(rc, -1);

        unsafe { tunnel_free(tunn) };
    }
}

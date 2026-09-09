use std::cell::RefCell;
use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::path::Path;

use nono::{AccessMode, CapabilitySet, Sandbox};

#[repr(C)]
pub enum NonoStatus {
    Ok = 0,
    ErrorGeneric = 1,
    ErrorUnsupported = 2,
    ErrorInvalidParam = 3,
    ErrorApplyFailed = 4,
    ErrorPermissionDenied = 5,
    ErrorAlreadyApplied = 6,
}

pub struct NonoCapabilitySet {
    pub read_paths: Vec<String>,
    pub write_paths: Vec<String>,
    pub devices: Vec<String>,
    pub allow_egress: bool,
    pub allow_loopback: bool,
    pub bind_port: u16,
    c_strings: Vec<CString>,
}

impl NonoCapabilitySet {
    fn new() -> Self {
        Self {
            read_paths: Vec::new(),
            write_paths: Vec::new(),
            devices: Vec::new(),
            allow_egress: false,
            allow_loopback: true,
            bind_port: 0,
            c_strings: Vec::new(),
        }
    }
}

thread_local! {
    static LAST_ERROR: RefCell<String> = RefCell::new(String::new());
    static LAST_ERROR_CSTR: RefCell<Option<CString>> = RefCell::new(None);
}

fn set_last_error(msg: &str) {
    LAST_ERROR.with(|e| {
        *e.borrow_mut() = msg.to_string();
    });
}

fn clear_last_error() {
    LAST_ERROR.with(|e| {
        e.borrow_mut().clear();
    });
}

#[no_mangle]
pub extern "C" fn nono_capability_set_new() -> *mut NonoCapabilitySet {
    clear_last_error();
    Box::into_raw(Box::new(NonoCapabilitySet::new()))
}

#[no_mangle]
pub unsafe extern "C" fn nono_capability_set_free(caps: *mut NonoCapabilitySet) {
    if !caps.is_null() {
        drop(Box::from_raw(caps));
    }
}

#[no_mangle]
pub unsafe extern "C" fn nono_capability_add_fs_read(
    caps: *mut NonoCapabilitySet,
    path: *const c_char,
) -> NonoStatus {
    if caps.is_null() || path.is_null() {
        set_last_error("Invalid capability set or path parameter");
        return NonoStatus::ErrorInvalidParam;
    }
    let c_str = CStr::from_ptr(path);
    match c_str.to_str() {
        Ok(s) if !s.is_empty() => {
            let caps_ref = &mut *caps;
            caps_ref.read_paths.push(s.to_string());
            NonoStatus::Ok
        }
        _ => {
            set_last_error("Invalid capability set or path parameter");
            NonoStatus::ErrorInvalidParam
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn nono_capability_add_fs_write(
    caps: *mut NonoCapabilitySet,
    path: *const c_char,
) -> NonoStatus {
    if caps.is_null() || path.is_null() {
        set_last_error("Invalid capability set or path parameter");
        return NonoStatus::ErrorInvalidParam;
    }
    let c_str = CStr::from_ptr(path);
    match c_str.to_str() {
        Ok(s) if !s.is_empty() => {
            let caps_ref = &mut *caps;
            caps_ref.write_paths.push(s.to_string());
            NonoStatus::Ok
        }
        _ => {
            set_last_error("Invalid capability set or path parameter");
            NonoStatus::ErrorInvalidParam
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn nono_capability_add_device(
    caps: *mut NonoCapabilitySet,
    dev_path: *const c_char,
) -> NonoStatus {
    if caps.is_null() || dev_path.is_null() {
        set_last_error("Invalid capability set or device path parameter");
        return NonoStatus::ErrorInvalidParam;
    }
    let c_str = CStr::from_ptr(dev_path);
    match c_str.to_str() {
        Ok(s) if !s.is_empty() => {
            let caps_ref = &mut *caps;
            caps_ref.devices.push(s.to_string());
            NonoStatus::Ok
        }
        _ => {
            set_last_error("Invalid capability set or device path parameter");
            NonoStatus::ErrorInvalidParam
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn nono_capability_set_network_egress(
    caps: *mut NonoCapabilitySet,
    allow_egress: bool,
) -> NonoStatus {
    if caps.is_null() {
        set_last_error("Invalid capability set parameter");
        return NonoStatus::ErrorInvalidParam;
    }
    let caps_ref = &mut *caps;
    caps_ref.allow_egress = allow_egress;
    NonoStatus::Ok
}

#[no_mangle]
pub unsafe extern "C" fn nono_capability_set_network_loopback(
    caps: *mut NonoCapabilitySet,
    allow_loopback: bool,
) -> NonoStatus {
    if caps.is_null() {
        set_last_error("Invalid capability set parameter");
        return NonoStatus::ErrorInvalidParam;
    }
    let caps_ref = &mut *caps;
    caps_ref.allow_loopback = allow_loopback;
    NonoStatus::Ok
}

#[no_mangle]
pub unsafe extern "C" fn nono_capability_set_bind_port(
    caps: *mut NonoCapabilitySet,
    port: u16,
) -> NonoStatus {
    if caps.is_null() {
        set_last_error("Invalid capability set parameter");
        return NonoStatus::ErrorInvalidParam;
    }
    let caps_ref = &mut *caps;
    caps_ref.bind_port = port;
    NonoStatus::Ok
}

#[no_mangle]
pub unsafe extern "C" fn nono_capability_get_read_path_count(
    caps: *const NonoCapabilitySet,
) -> libc::size_t {
    if caps.is_null() {
        0
    } else {
        let caps_ref = &*caps;
        caps_ref.read_paths.len()
    }
}

#[no_mangle]
pub unsafe extern "C" fn nono_capability_get_read_path(
    caps: *mut NonoCapabilitySet,
    index: libc::size_t,
) -> *const c_char {
    if caps.is_null() {
        return std::ptr::null();
    }
    let caps_ref = &mut *caps;
    if index >= caps_ref.read_paths.len() {
        return std::ptr::null();
    }
    if let Ok(c) = CString::new(caps_ref.read_paths[index].clone()) {
        let ptr = c.as_ptr();
        caps_ref.c_strings.push(c);
        ptr
    } else {
        std::ptr::null()
    }
}

#[no_mangle]
pub unsafe extern "C" fn nono_capability_get_write_path_count(
    caps: *const NonoCapabilitySet,
) -> libc::size_t {
    if caps.is_null() {
        0
    } else {
        let caps_ref = &*caps;
        caps_ref.write_paths.len()
    }
}

#[no_mangle]
pub unsafe extern "C" fn nono_capability_get_write_path(
    caps: *mut NonoCapabilitySet,
    index: libc::size_t,
) -> *const c_char {
    if caps.is_null() {
        return std::ptr::null();
    }
    let caps_ref = &mut *caps;
    if index >= caps_ref.write_paths.len() {
        return std::ptr::null();
    }
    if let Ok(c) = CString::new(caps_ref.write_paths[index].clone()) {
        let ptr = c.as_ptr();
        caps_ref.c_strings.push(c);
        ptr
    } else {
        std::ptr::null()
    }
}

#[no_mangle]
pub unsafe extern "C" fn nono_capability_get_device_count(
    caps: *const NonoCapabilitySet,
) -> libc::size_t {
    if caps.is_null() {
        0
    } else {
        let caps_ref = &*caps;
        caps_ref.devices.len()
    }
}

#[no_mangle]
pub unsafe extern "C" fn nono_capability_get_device(
    caps: *mut NonoCapabilitySet,
    index: libc::size_t,
) -> *const c_char {
    if caps.is_null() {
        return std::ptr::null();
    }
    let caps_ref = &mut *caps;
    if index >= caps_ref.devices.len() {
        return std::ptr::null();
    }
    if let Ok(c) = CString::new(caps_ref.devices[index].clone()) {
        let ptr = c.as_ptr();
        caps_ref.c_strings.push(c);
        ptr
    } else {
        std::ptr::null()
    }
}

#[no_mangle]
pub unsafe extern "C" fn nono_capability_get_network_egress(
    caps: *const NonoCapabilitySet,
) -> bool {
    if caps.is_null() {
        false
    } else {
        let caps_ref = &*caps;
        caps_ref.allow_egress
    }
}

#[no_mangle]
pub unsafe extern "C" fn nono_capability_get_network_loopback(
    caps: *const NonoCapabilitySet,
) -> bool {
    if caps.is_null() {
        false
    } else {
        let caps_ref = &*caps;
        caps_ref.allow_loopback
    }
}

#[no_mangle]
pub unsafe extern "C" fn nono_capability_get_bind_port(
    caps: *const NonoCapabilitySet,
) -> u16 {
    if caps.is_null() {
        0
    } else {
        let caps_ref = &*caps;
        caps_ref.bind_port
    }
}

fn add_grant_to_caps(caps: CapabilitySet, path_str: &str, mode: AccessMode) -> CapabilitySet {
    let p = Path::new(path_str);
    if p.exists() {
        if p.is_dir() {
            if let Ok(c) = caps.clone().allow_path(p, mode) {
                return c;
            }
        } else if let Ok(c) = caps.clone().allow_file(p, mode) {
            return c;
        }
    }
    caps
}

#[no_mangle]
pub unsafe extern "C" fn nono_sandbox_apply(caps: *const NonoCapabilitySet) -> NonoStatus {
    if caps.is_null() {
        set_last_error("Invalid capability set parameter");
        return NonoStatus::ErrorInvalidParam;
    }

    let caps_ref = &*caps;
    let mut rust_caps = CapabilitySet::new();

    for p in &caps_ref.read_paths {
        rust_caps = add_grant_to_caps(rust_caps, p, AccessMode::Read);
    }
    for p in &caps_ref.write_paths {
        rust_caps = add_grant_to_caps(rust_caps, p, AccessMode::ReadWrite);
    }
    for d in &caps_ref.devices {
        rust_caps = add_grant_to_caps(rust_caps, d, AccessMode::ReadWrite);
    }

    if !caps_ref.allow_egress {
        rust_caps = rust_caps.block_network();
    }

    if caps_ref.bind_port > 0 {
        if caps_ref.allow_loopback {
            rust_caps = rust_caps.allow_localhost_port(caps_ref.bind_port);
        } else {
            rust_caps = rust_caps.allow_tcp_bind(caps_ref.bind_port);
        }
    }

    match Sandbox::apply_auto(&rust_caps) {
        Ok(_) => {
            clear_last_error();
            NonoStatus::Ok
        }
        Err(e) => {
            // On older kernels (Landlock ABI < V4), network port exceptions cannot be
            // applied via Landlock. Retry with pure filesystem Landlock isolation + seccomp
            // so the child process retains full filesystem confinement.
            let mut fs_caps = CapabilitySet::new();
            for p in &caps_ref.read_paths {
                fs_caps = add_grant_to_caps(fs_caps, p, AccessMode::Read);
            }
            for p in &caps_ref.write_paths {
                fs_caps = add_grant_to_caps(fs_caps, p, AccessMode::ReadWrite);
            }
            for d in &caps_ref.devices {
                fs_caps = add_grant_to_caps(fs_caps, d, AccessMode::ReadWrite);
            }
            if !caps_ref.allow_egress {
                fs_caps = fs_caps.block_network();
            }

            if Sandbox::apply_auto(&fs_caps).is_ok() {
                clear_last_error();
                return NonoStatus::Ok;
            }

            let err_msg = format!("nono sandbox application failed: {}", e);
            set_last_error(&err_msg);
            match e {
                nono::NonoError::UnsupportedPlatform(_) => NonoStatus::ErrorUnsupported,
                _ => NonoStatus::ErrorApplyFailed,
            }
        }
    }
}

#[no_mangle]
pub extern "C" fn nono_is_supported() -> bool {
    Sandbox::is_supported()
}

#[no_mangle]
pub extern "C" fn nono_get_backend_name() -> *const c_char {
    let info = Sandbox::support_info();
    match info.platform {
        "linux" | "Linux" => b"nono-landlock\0".as_ptr() as *const c_char,
        "macos" | "macOS" => b"nono-seatbelt\0".as_ptr() as *const c_char,
        _ => b"nono-fallback\0".as_ptr() as *const c_char,
    }
}

#[no_mangle]
pub extern "C" fn nono_status_to_string(status: NonoStatus) -> *const c_char {
    match status {
        NonoStatus::Ok => b"NONO_OK\0".as_ptr() as *const c_char,
        NonoStatus::ErrorGeneric => b"NONO_ERROR_GENERIC\0".as_ptr() as *const c_char,
        NonoStatus::ErrorUnsupported => b"NONO_ERROR_UNSUPPORTED\0".as_ptr() as *const c_char,
        NonoStatus::ErrorInvalidParam => b"NONO_ERROR_INVALID_PARAM\0".as_ptr() as *const c_char,
        NonoStatus::ErrorApplyFailed => b"NONO_ERROR_APPLY_FAILED\0".as_ptr() as *const c_char,
        NonoStatus::ErrorPermissionDenied => b"NONO_ERROR_PERMISSION_DENIED\0".as_ptr() as *const c_char,
        NonoStatus::ErrorAlreadyApplied => b"NONO_ERROR_ALREADY_APPLIED\0".as_ptr() as *const c_char,
    }
}

#[no_mangle]
pub extern "C" fn nono_get_last_error() -> *const c_char {
    LAST_ERROR.with(|e| {
        let err = e.borrow();
        LAST_ERROR_CSTR.with(|c_cell| {
            if let Ok(c) = CString::new(err.clone()) {
                let ptr = c.as_ptr();
                *c_cell.borrow_mut() = Some(c);
                ptr
            } else {
                b"\0".as_ptr() as *const c_char
            }
        })
    })
}

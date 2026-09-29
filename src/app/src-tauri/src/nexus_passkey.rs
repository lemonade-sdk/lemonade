use authenticator::{
    authenticatorservice::{AuthenticatorService, RegisterArgs, SignArgs},
    crypto::{COSEAlgorithm, COSEKeyType},
    ctap2::server::{
        AuthenticationExtensionsClientInputs, PublicKeyCredentialDescriptor,
        PublicKeyCredentialParameters, PublicKeyCredentialUserEntity, RelyingParty,
        ResidentKeyRequirement, Transport, UserVerificationRequirement,
    },
    statecallback::StateCallback,
    Pin, StatusPinUv, StatusUpdate,
};
use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use serde::Deserialize;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::sync::{
    atomic::{AtomicBool, Ordering},
    mpsc::{channel, Sender},
    Arc, Mutex,
};
use std::time::{Duration, Instant};
use tauri::{Emitter, Manager};

#[derive(Default)]
pub struct PasskeyState {
    running: AtomicBool,
    cancelled: AtomicBool,
    pin: Mutex<Option<Sender<Pin>>>,
}

#[derive(Deserialize)]
pub struct PasskeyOptions {
    challenge: String,
    rp_id: String,
    user_id: String,
    credential_id: String,
    device_name: String,
    registration: bool,
}

fn check_options(options: &PasskeyOptions) -> Result<Vec<u8>, String> {
    let rp = &options.rp_id;
    if rp.is_empty()
        || rp.len() > 253
        || !rp
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b'-')
    {
        return Err("Invalid passkey domain".into());
    }
    let challenge = URL_SAFE_NO_PAD
        .decode(&options.challenge)
        .map_err(|_| "Invalid passkey challenge")?;
    if challenge.len() < 16 || challenge.len() > 1024 {
        return Err("Invalid passkey challenge".into());
    }
    Ok(challenge)
}

fn perform(app: tauri::AppHandle, options: PasskeyOptions) -> Result<Value, String> {
    check_options(&options)?;
    let credential_id = if options.registration {
        vec![]
    } else {
        URL_SAFE_NO_PAD
            .decode(&options.credential_id)
            .map_err(|_| "Invalid credential ID")?
    };
    let state = app.state::<PasskeyState>();
    let origin = format!("https://{}", options.rp_id);
    let client_data = serde_json::to_vec(&json!({
        "type": if options.registration { "webauthn.create" } else { "webauthn.get" },
        "challenge": options.challenge, "origin": origin, "crossOrigin": false,
    }))
    .map_err(|_| "Cannot encode passkey challenge")?;
    let hash: [u8; 32] = Sha256::digest(&client_data).into();
    let mut service =
        AuthenticatorService::new().map_err(|_| "Cannot start the security key service")?;
    service.add_u2f_usb_hid_platform_transports();
    let (status_tx, status_rx) = channel();
    let finished = Arc::new(AtomicBool::new(false));
    let status_done = finished.clone();
    let event_app = app.clone();
    let status_thread = std::thread::spawn(move || {
        while !status_done.load(Ordering::Acquire) {
            let status = match status_rx.recv_timeout(Duration::from_millis(200)) {
                Ok(status) => status,
                Err(std::sync::mpsc::RecvTimeoutError::Timeout) => continue,
                Err(_) => break,
            };
            let state = event_app.state::<PasskeyState>();
            let (message, needs_pin) = match status {
                StatusUpdate::PresenceRequired | StatusUpdate::SelectDeviceNotice => ("Touch your FIDO2 security key.".to_string(), false),
                StatusUpdate::PinUvError(StatusPinUv::PinRequired(sender)) => {
                    *state.pin.lock().unwrap_or_else(|e| e.into_inner()) = Some(sender);
                    ("Enter your security key PIN.".to_string(), true)
                }
                StatusUpdate::PinUvError(StatusPinUv::InvalidPin(sender, attempts)) => {
                    *state.pin.lock().unwrap_or_else(|e| e.into_inner()) = Some(sender);
                    (format!("Incorrect PIN. {}", attempts.map(|n| format!("{n} attempts remaining.")).unwrap_or_default()), true)
                }
                StatusUpdate::PinUvError(StatusPinUv::InvalidUv(_)) => ("Verification failed. Touch your key again.".to_string(), false),
                StatusUpdate::PinUvError(_) => ("Security key verification is unavailable. Check that the key has a PIN and is not blocked.".to_string(), false),
                StatusUpdate::SelectResultNotice(sender, _) => { let _ = sender.send(None); continue; }
                StatusUpdate::InteractiveManagement(_) => continue,
            };
            let _ = event_app.emit_to(
                "main",
                "nexus:passkey-status",
                json!({"message": message, "needs_pin": needs_pin}),
            );
        }
    });
    let (result_tx, result_rx) = channel::<Result<Value, String>>();
    let start = if options.registration {
        service.register(60000, RegisterArgs {
            client_data_hash: hash,
            relying_party: RelyingParty { id: options.rp_id.clone(), name: Some("Lemonade Nexus".into()) },
            origin: origin.clone(),
            user: PublicKeyCredentialUserEntity { id: options.user_id.as_bytes().to_vec(), name: Some(options.user_id), display_name: Some(options.device_name) },
            pub_cred_params: vec![PublicKeyCredentialParameters { alg: COSEAlgorithm::ES256 }],
            exclude_list: vec![], user_verification_req: UserVerificationRequirement::Required,
            resident_key_req: ResidentKeyRequirement::Discouraged,
            extensions: AuthenticationExtensionsClientInputs::default(), pin: None, use_ctap1_fallback: false,
        }, status_tx, StateCallback::new(Box::new(move |result: authenticator::Result<authenticator::RegisterResult>| {
            let value = result.map_err(|_| "Passkey registration failed or was cancelled".to_string()).and_then(|result| {
                let data = result.att_obj.auth_data;
                if data.flags.bits() & 5 != 5 { return Err("User verification is required".into()); }
                let credential = data.credential_data.ok_or("Security key returned no credential")?;
                match credential.credential_public_key.key {
                    COSEKeyType::EC2(key) if key.x.len() == 32 && key.y.len() == 32 => {
                        let hex = |bytes: Vec<u8>| bytes.iter().map(|b| format!("{b:02x}")).collect::<String>();
                        Ok(json!({"credential_id": URL_SAFE_NO_PAD.encode(credential.credential_id), "public_key_x": hex(key.x), "public_key_y": hex(key.y)}))
                    }
                    _ => Err("Security key did not create a P-256 credential".into()),
                }
            });
            let _ = result_tx.send(value);
        })))
    } else {
        service.sign(60000, SignArgs {
            client_data_hash: hash, origin, relying_party_id: options.rp_id,
            allow_list: vec![PublicKeyCredentialDescriptor { id: credential_id, transports: vec![Transport::USB] }],
            user_verification_req: UserVerificationRequirement::Required, user_presence_req: true,
            extensions: AuthenticationExtensionsClientInputs::default(), pin: None, use_ctap1_fallback: false,
        }, status_tx, StateCallback::new(Box::new(move |result: authenticator::Result<authenticator::SignResult>| {
            let value = result.map_err(|_| "Passkey verification failed or was cancelled".to_string()).and_then(|result| {
                let assertion = result.assertion;
                if assertion.auth_data.flags.bits() & 5 != 5 { return Err("User verification is required".into()); }
                let id = assertion.credentials.ok_or("Security key returned no credential")?.id;
                Ok(json!({"credential_id": URL_SAFE_NO_PAD.encode(id), "authenticator_data": URL_SAFE_NO_PAD.encode(assertion.auth_data.to_vec()),
                    "signature": URL_SAFE_NO_PAD.encode(assertion.signature), "client_data_json": URL_SAFE_NO_PAD.encode(client_data)}))
            });
            let _ = result_tx.send(value);
        })))
    };
    let deadline = Instant::now() + Duration::from_secs(65);
    let result = if start.is_err() {
        Err("Cannot start passkey verification".into())
    } else {
        loop {
            if state.cancelled.load(Ordering::Acquire) || Instant::now() >= deadline {
                break Err("Passkey operation cancelled or timed out".into());
            }
            match result_rx.recv_timeout(Duration::from_millis(200)) {
                Ok(value) => break value,
                Err(std::sync::mpsc::RecvTimeoutError::Timeout) => continue,
                Err(_) => break Err("Security key disconnected".into()),
            }
        }
    };
    let _ = service.cancel();
    finished.store(true, Ordering::Release);
    let _ = status_thread.join();
    *state.pin.lock().unwrap_or_else(|e| e.into_inner()) = None;
    result
}

#[tauri::command]
pub async fn nexus_passkey(
    app: tauri::AppHandle,
    window: tauri::WebviewWindow,
    options: PasskeyOptions,
) -> Result<Value, String> {
    if window.label() != "main" {
        return Err("Passkeys are available in the main window only".into());
    }
    check_options(&options)?;
    let state = app.state::<PasskeyState>();
    if state.running.swap(true, Ordering::AcqRel) {
        return Err("Another passkey operation is running".into());
    }
    state.cancelled.store(false, Ordering::Release);
    let worker_app = app.clone();
    let result = tauri::async_runtime::spawn_blocking(move || perform(worker_app, options)).await;
    state.running.store(false, Ordering::Release);
    result.map_err(|_| "Passkey operation failed".to_string())?
}

#[tauri::command]
pub fn nexus_passkey_pin(
    app: tauri::AppHandle,
    window: tauri::WebviewWindow,
    pin: String,
) -> Result<(), String> {
    if window.label() != "main" {
        return Err("Invalid window".into());
    }
    if pin.len() < 4 || pin.len() > 63 {
        return Err("Enter a PIN between 4 and 63 bytes".into());
    }
    app.state::<PasskeyState>()
        .pin
        .lock()
        .map_err(|_| "Passkey service unavailable")?
        .take()
        .ok_or("No PIN requested")?
        .send(Pin::new(&pin))
        .map_err(|_| "Passkey request expired".into())
}

#[tauri::command]
pub fn nexus_passkey_cancel(
    app: tauri::AppHandle,
    window: tauri::WebviewWindow,
) -> Result<(), String> {
    if window.label() != "main" {
        return Err("Invalid window".into());
    }
    app.state::<PasskeyState>()
        .cancelled
        .store(true, Ordering::Release);
    Ok(())
}

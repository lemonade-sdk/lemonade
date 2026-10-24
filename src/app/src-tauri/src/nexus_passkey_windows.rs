use super::{check_options, PasskeyOptions, PasskeyState};
use authenticator::{
    crypto::{COSEAlgorithm, COSEKeyType, Curve},
    ctap2::attestation::AuthenticatorData,
};
use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use serde::Deserialize;
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::sync::atomic::Ordering;
use tauri::{Emitter, Manager};
use windows::{
    core::{w, PCWSTR},
    Win32::{Foundation::HWND, Networking::WindowsWebServices::*},
};

struct Cancellation<'a>(&'a PasskeyState);

impl Drop for Cancellation<'_> {
    fn drop(&mut self) {
        *self
            .0
            .cancellation
            .lock()
            .unwrap_or_else(|e| e.into_inner()) = None;
    }
}

fn verified_data<'a>(bytes: &'a [u8], rp_id: &str) -> Result<&'a [u8], String> {
    if bytes.len() < 37 || bytes[..32] != Sha256::digest(rp_id.as_bytes())[..] {
        return Err("The passkey returned an invalid relying party".into());
    }
    if bytes[32] & 5 != 5 {
        return Err("User verification is required".into());
    }
    Ok(bytes)
}

fn registration_reply(bytes: &[u8], rp_id: &str) -> Result<Value, String> {
    let bytes = verified_data(bytes, rp_id)?;
    let data = AuthenticatorData::deserialize(serde::de::value::BorrowedBytesDeserializer::<
        serde::de::value::Error,
    >::new(bytes))
    .map_err(|_| "The passkey returned invalid credential data")?;
    let credential = data
        .credential_data
        .ok_or("The passkey returned no credential")?;
    if credential.credential_id.is_empty()
        || credential.credential_public_key.alg != COSEAlgorithm::ES256
    {
        return Err("The passkey did not create a P-256 credential".into());
    }
    match credential.credential_public_key.key {
        COSEKeyType::EC2(key)
            if key.curve == Curve::SECP256R1 && key.x.len() == 32 && key.y.len() == 32 =>
        {
            let hex = |bytes: Vec<u8>| bytes.iter().map(|b| format!("{b:02x}")).collect::<String>();
            Ok(
                json!({"credential_id": URL_SAFE_NO_PAD.encode(credential.credential_id),
                "public_key_x": hex(key.x), "public_key_y": hex(key.y)}),
            )
        }
        _ => Err("The passkey did not create a P-256 credential".into()),
    }
}

pub(super) fn cancel(state: &PasskeyState) -> Result<(), String> {
    if let Some(id) = *state.cancellation.lock().unwrap_or_else(|e| e.into_inner()) {
        unsafe { WebAuthNCancelCurrentOperation(&id) }
            .map_err(|_| "Cannot cancel the Windows passkey request")?;
    }
    Ok(())
}

pub(super) fn perform(
    app: tauri::AppHandle,
    hwnd: isize,
    options: PasskeyOptions,
) -> Result<Value, String> {
    check_options(&options)?;
    let state = app.state::<PasskeyState>();
    let mut cancellation = unsafe { WebAuthNGetCancellationId() }
        .map_err(|_| "Cannot start the Windows passkey service")?;
    *state.cancellation.lock().unwrap_or_else(|e| e.into_inner()) = Some(cancellation);
    let _cancellation = Cancellation(&state);
    if state.cancelled.load(Ordering::Acquire) {
        return Err("Passkey operation cancelled or timed out".into());
    }
    let _ = app.emit_to(
        "main",
        "nexus:passkey-status",
        json!({
            "message": "Complete verification in the Windows passkey dialog.", "needs_pin": false
        }),
    );
    let rp_id: Vec<u16> = options.rp_id.encode_utf16().chain(Some(0)).collect();
    let display_name: Vec<u16> = options.device_name.encode_utf16().chain(Some(0)).collect();
    let user_name: Vec<u16> = options.user_id.encode_utf16().chain(Some(0)).collect();
    let mut user_id = options.user_id.as_bytes().to_vec();
    let mut client_json = serde_json::to_vec(&json!({
        "type": if options.registration { "webauthn.create" } else { "webauthn.get" },
        "challenge": options.challenge, "origin": format!("https://{}", options.rp_id), "crossOrigin": false,
    })).map_err(|_| "Cannot encode passkey challenge")?;
    let client = WEBAUTHN_CLIENT_DATA {
        dwVersion: WEBAUTHN_CLIENT_DATA_CURRENT_VERSION,
        cbClientDataJSON: client_json.len() as u32,
        pbClientDataJSON: client_json.as_mut_ptr(),
        pwszHashAlgId: w!("SHA-256"),
    };
    let hwnd = HWND(hwnd as *mut _);
    if options.registration {
        let rp = WEBAUTHN_RP_ENTITY_INFORMATION {
            dwVersion: WEBAUTHN_RP_ENTITY_INFORMATION_CURRENT_VERSION,
            pwszId: PCWSTR(rp_id.as_ptr()),
            pwszName: w!("Lemonade Nexus"),
            ..Default::default()
        };
        let user = WEBAUTHN_USER_ENTITY_INFORMATION {
            dwVersion: WEBAUTHN_USER_ENTITY_INFORMATION_CURRENT_VERSION,
            cbId: user_id.len() as u32,
            pbId: user_id.as_mut_ptr(),
            pwszName: PCWSTR(user_name.as_ptr()),
            pwszDisplayName: PCWSTR(display_name.as_ptr()),
            ..Default::default()
        };
        let mut algorithm = WEBAUTHN_COSE_CREDENTIAL_PARAMETER {
            dwVersion: WEBAUTHN_COSE_CREDENTIAL_PARAMETER_CURRENT_VERSION,
            pwszCredentialType: w!("public-key"),
            lAlg: WEBAUTHN_COSE_ALGORITHM_ECDSA_P256_WITH_SHA256,
        };
        let algorithms = WEBAUTHN_COSE_CREDENTIAL_PARAMETERS {
            cCredentialParameters: 1,
            pCredentialParameters: &mut algorithm,
        };
        let settings = WEBAUTHN_AUTHENTICATOR_MAKE_CREDENTIAL_OPTIONS {
            dwVersion: WEBAUTHN_AUTHENTICATOR_MAKE_CREDENTIAL_OPTIONS_VERSION_2,
            dwTimeoutMilliseconds: 60000,
            dwAuthenticatorAttachment: WEBAUTHN_AUTHENTICATOR_ATTACHMENT_ANY,
            dwUserVerificationRequirement: WEBAUTHN_USER_VERIFICATION_REQUIREMENT_REQUIRED,
            dwAttestationConveyancePreference: WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_NONE,
            pCancellationId: &mut cancellation,
            ..Default::default()
        };
        let credential = unsafe {
            WebAuthNAuthenticatorMakeCredential(
                hwnd,
                &rp,
                &user,
                &algorithms,
                &client,
                Some(&settings),
            )
        }
        .map_err(|_| "Passkey registration failed or was cancelled")?;
        if credential.is_null() {
            return Err("The passkey returned no credential".into());
        }
        let result = unsafe {
            let credential = &*credential;
            if credential.pbAuthenticatorData.is_null() || credential.cbAuthenticatorData < 37 {
                Err("The passkey returned invalid credential data".into())
            } else {
                registration_reply(
                    std::slice::from_raw_parts(
                        credential.pbAuthenticatorData,
                        credential.cbAuthenticatorData as usize,
                    ),
                    &options.rp_id,
                )
            }
        };
        unsafe { WebAuthNFreeCredentialAttestation(Some(credential)) };
        result
    } else {
        let mut id = URL_SAFE_NO_PAD
            .decode(&options.credential_id)
            .map_err(|_| "Invalid credential ID")?;
        if id.is_empty() {
            return Err("Invalid credential ID".into());
        }
        let mut credential = WEBAUTHN_CREDENTIAL {
            dwVersion: WEBAUTHN_CREDENTIAL_CURRENT_VERSION,
            cbId: id.len() as u32,
            pbId: id.as_mut_ptr(),
            pwszCredentialType: w!("public-key"),
        };
        let settings = WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS {
            dwVersion: WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS_VERSION_2,
            dwTimeoutMilliseconds: 60000,
            CredentialList: WEBAUTHN_CREDENTIALS {
                cCredentials: 1,
                pCredentials: &mut credential,
            },
            dwAuthenticatorAttachment: WEBAUTHN_AUTHENTICATOR_ATTACHMENT_ANY,
            dwUserVerificationRequirement: WEBAUTHN_USER_VERIFICATION_REQUIREMENT_REQUIRED,
            pCancellationId: &mut cancellation,
            ..Default::default()
        };
        let assertion = unsafe {
            WebAuthNAuthenticatorGetAssertion(
                hwnd,
                PCWSTR(rp_id.as_ptr()),
                &client,
                Some(&settings),
            )
        }
        .map_err(|_| "Passkey verification failed or was cancelled")?;
        if assertion.is_null() {
            return Err("The passkey returned no credential".into());
        }
        let result = unsafe {
            let assertion = &*assertion;
            if assertion.pbAuthenticatorData.is_null()
                || assertion.pbSignature.is_null()
                || assertion.Credential.pbId.is_null()
                || assertion.cbAuthenticatorData < 37
                || assertion.cbSignature == 0
            {
                Err("The passkey returned invalid credential data".into())
            } else {
                let data = std::slice::from_raw_parts(
                    assertion.pbAuthenticatorData,
                    assertion.cbAuthenticatorData as usize,
                );
                let returned_id = std::slice::from_raw_parts(
                    assertion.Credential.pbId,
                    assertion.Credential.cbId as usize,
                );
                if returned_id != id {
                    Err("The passkey returned a different credential".into())
                } else {
                    verified_data(data, &options.rp_id).map(|_| json!({
                    "credential_id": URL_SAFE_NO_PAD.encode(returned_id), "authenticator_data": URL_SAFE_NO_PAD.encode(data),
                    "signature": URL_SAFE_NO_PAD.encode(std::slice::from_raw_parts(assertion.pbSignature, assertion.cbSignature as usize)),
                    "client_data_json": URL_SAFE_NO_PAD.encode(&client_json),
                }))
                }
            }
        };
        unsafe { WebAuthNFreeAssertion(assertion) };
        result
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_wrong_relying_party_and_missing_verification() {
        let mut data = Sha256::digest(b"nexus.example").to_vec();
        data.extend_from_slice(&[5, 0, 0, 0, 0]);
        assert!(verified_data(&data, "nexus.example").is_ok());
        assert!(verified_data(&data, "other.example").is_err());
        data[32] = 1;
        assert!(verified_data(&data, "nexus.example").is_err());
        data[32] = 4;
        assert!(verified_data(&data, "nexus.example").is_err());
        assert!(verified_data(&data[..36], "nexus.example").is_err());
    }

    #[test]
    fn converts_attested_p256_credentials_and_rejects_truncation() {
        let mut data = Sha256::digest(b"nexus.example").to_vec();
        data.extend_from_slice(&[0x45, 0, 0, 0, 0]);
        data.extend_from_slice(&[0; 16]);
        data.extend_from_slice(&[0, 3, 1, 2, 3]);
        data.extend_from_slice(&[0xa5, 1, 2, 3, 0x26, 0x20, 1, 0x21, 0x58, 32]);
        data.extend_from_slice(&[1; 32]);
        data.extend_from_slice(&[0x22, 0x58, 32]);
        data.extend_from_slice(&[2; 32]);
        let reply = registration_reply(&data, "nexus.example").unwrap();
        assert_eq!(reply["credential_id"], "AQID");
        assert_eq!(reply["public_key_x"], "01".repeat(32));
        assert_eq!(reply["public_key_y"], "02".repeat(32));
        assert!(registration_reply(&data[..data.len() - 1], "nexus.example").is_err());
        assert!(registration_reply(&data, "other.example").is_err());
        data[32] = 0x41;
        assert!(registration_reply(&data, "nexus.example").is_err());
    }

    #[test]
    fn clears_the_cancellation_id_when_an_operation_ends() {
        let state = PasskeyState::default();
        *state.cancellation.lock().unwrap() = Some(windows::core::GUID::zeroed());
        {
            let _operation = Cancellation(&state);
        }
        assert!(state.cancellation.lock().unwrap().is_none());
    }
}

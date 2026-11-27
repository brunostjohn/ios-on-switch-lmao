// SPDX-License-Identifier: GPL-3.0-or-later
//! Apple Encrypted Archive WKMS auth data parsing and RFC 9180 HPKE key unwrap.
//!
//! Encrypted IPSW members carry `com.apple.wkms.fcs-key-url` and `com.apple.wkms.fcs-response` in their AEA1 auth
//! data. The installer fetches the P-256 private key PEM from Apple at install time and unwraps the 32-byte symmetric
//! key via HPKE Base mode (DHKEM(P-256, HKDF-SHA256), HKDF-SHA256, AES-256-GCM).

use std::collections::BTreeMap;
use std::io::{Read, Seek, SeekFrom};
use std::time::Duration;

use aws_lc_rs::aead::{AES_256_GCM, Aad, LessSafeKey, Nonce, UnboundKey};
use aws_lc_rs::agreement::{self, ECDH_P256, PrivateKey, UnparsedPublicKey};
use base64::Engine;
use base64::engine::general_purpose::STANDARD;
use hkdf::Hkdf;
use reqwest::Url;
use serde::Deserialize;
use sha2::Sha256;

use crate::{Error, Result};

const MAGIC: &[u8; 4] = b"AEA1";
const PROFILE_SYMMETRIC: u32 = 1;
const PROLOGUE_LEN: usize = 12;
const MAX_AUTH_DATA_LEN: usize = 1 << 20;
const MAX_PEM_BYTES: u64 = 64 << 10;

const FIELD_FCS_KEY_URL: &str = "com.apple.wkms.fcs-key-url";
const FIELD_FCS_RESPONSE: &str = "com.apple.wkms.fcs-response";

const HPKE_VERSION: &[u8] = b"HPKE-v1";
const KEM_SUITE_ID: &[u8] = b"KEM\x00\x10";
const HPKE_SUITE_ID: &[u8] = b"HPKE\x00\x10\x00\x01\x00\x02";
const MODE_BASE: u8 = 0x00;

const P256_UNCOMPRESSED_LEN: usize = 65;
const GCM_TAG_LEN: usize = 16;

const HTTP_CONNECT_TIMEOUT: Duration = Duration::from_secs(30);
const HTTP_TIMEOUT: Duration = Duration::from_secs(60);

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AuthData {
    pub fcs_key_url: String,
    pub enc_request: [u8; P256_UNCOMPRESSED_LEN],
    pub wrapped_key: Vec<u8>,
}

#[derive(Deserialize)]
struct FcsResponse {
    #[serde(rename = "enc-request")]
    enc_request: String,
    #[serde(rename = "wrapped-key")]
    wrapped_key: String,
}

pub fn read_auth_data<R: Read + Seek>(archive: &mut R) -> Result<AuthData> {
    let mut head = [0u8; PROLOGUE_LEN];
    archive.seek(SeekFrom::Start(0)).map_err(Error::Read)?;
    archive.read_exact(&mut head).map_err(Error::Read)?;
    if &head[..4] != MAGIC {
        return Err(Error::Unsupported("no AEA1 magic".into()));
    }
    let profile = u32::from_le_bytes([head[4], head[5], head[6], 0]);
    if profile != PROFILE_SYMMETRIC {
        return Err(Error::Unsupported(format!(
            "profile {profile}; only 1 (symmetric key, unsigned) is read"
        )));
    }
    let auth_len = u32::from_le_bytes(head[8..12].try_into().unwrap()) as usize;
    if auth_len == 0 || auth_len > MAX_AUTH_DATA_LEN {
        return Err(Error::AuthData(format!("auth data length {auth_len} out of range")));
    }
    let mut blob = vec![0u8; auth_len];
    archive.read_exact(&mut blob).map_err(Error::Read)?;
    parse_auth_fields(&blob)
}

pub fn parse_auth_fields(blob: &[u8]) -> Result<AuthData> {
    let mut fields: BTreeMap<&str, &[u8]> = BTreeMap::new();
    let mut at = 0usize;
    while at < blob.len() {
        if at + 4 > blob.len() {
            return Err(Error::AuthData(format!("truncated auth field header at {at}")));
        }
        let len = u32::from_le_bytes(blob[at..at + 4].try_into().unwrap()) as usize;
        let end = at
            .checked_add(len)
            .filter(|&end| len >= 5 && end <= blob.len())
            .ok_or_else(|| Error::AuthData(format!("bad auth field length {len} at {at}")))?;
        let body = &blob[at + 4..end];
        let sep = body
            .iter()
            .position(|&b| b == 0)
            .filter(|&pos| pos > 0)
            .ok_or_else(|| Error::AuthData(format!("auth field at {at} has no NUL-terminated key")))?;
        let key = std::str::from_utf8(&body[..sep])
            .map_err(|_| Error::AuthData(format!("auth field key at {at} is not UTF-8")))?;
        fields.insert(key, &body[sep + 1..]);
        at = end;
    }

    let url_bytes = fields
        .get(FIELD_FCS_KEY_URL)
        .ok_or_else(|| Error::AuthData(format!("missing {FIELD_FCS_KEY_URL}")))?;
    let fcs_key_url = std::str::from_utf8(url_bytes)
        .map_err(|_| Error::AuthData(format!("{FIELD_FCS_KEY_URL} is not UTF-8")))?
        .trim()
        .to_owned();
    if fcs_key_url.is_empty() {
        return Err(Error::AuthData(format!("{FIELD_FCS_KEY_URL} is empty")));
    }

    let resp_bytes = fields
        .get(FIELD_FCS_RESPONSE)
        .ok_or_else(|| Error::AuthData(format!("missing {FIELD_FCS_RESPONSE}")))?;
    let response: FcsResponse =
        serde_json::from_slice(resp_bytes).map_err(|e| Error::AuthData(format!("{FIELD_FCS_RESPONSE}: {e}")))?;

    let enc_vec = STANDARD
        .decode(response.enc_request.trim())
        .map_err(|e| Error::AuthData(format!("enc-request base64: {e}")))?;
    let enc_request: [u8; P256_UNCOMPRESSED_LEN] = enc_vec
        .try_into()
        .map_err(|v: Vec<u8>| Error::AuthData(format!("enc-request is {} bytes, expected 65", v.len())))?;
    if enc_request[0] != 0x04 {
        return Err(Error::AuthData("enc-request is not an uncompressed P-256 point".into()));
    }

    let wrapped_key = STANDARD
        .decode(response.wrapped_key.trim())
        .map_err(|e| Error::AuthData(format!("wrapped-key base64: {e}")))?;
    if wrapped_key.len() < GCM_TAG_LEN || wrapped_key.len() > 256 {
        return Err(Error::AuthData(format!(
            "wrapped-key length {} out of range",
            wrapped_key.len()
        )));
    }

    Ok(AuthData {
        fcs_key_url,
        enc_request,
        wrapped_key,
    })
}

pub fn validate_fcs_url(raw: &str) -> Result<Url> {
    let url = Url::parse(raw).map_err(|e| Error::KeyFetch {
        url: raw.to_owned(),
        reason: format!("invalid URL: {e}"),
    })?;
    if url.scheme() != "https" {
        return Err(Error::KeyFetch {
            url: raw.to_owned(),
            reason: format!("refusing non-HTTPS scheme {:?}", url.scheme()),
        });
    }
    if !url.username().is_empty() || url.password().is_some() {
        return Err(Error::KeyFetch {
            url: raw.to_owned(),
            reason: "refusing URL with userinfo".into(),
        });
    }
    let host = url.host_str().unwrap_or("");
    if host != "apple.com" && !host.ends_with(".apple.com") {
        return Err(Error::KeyFetch {
            url: raw.to_owned(),
            reason: format!("refusing non-Apple key host {host:?}"),
        });
    }
    Ok(url)
}

pub fn fetch_fcs_key_pem(url: &str) -> Result<Vec<u8>> {
    let parsed = validate_fcs_url(url)?;
    let client = reqwest::blocking::Client::builder()
        .connect_timeout(HTTP_CONNECT_TIMEOUT)
        .timeout(HTTP_TIMEOUT)
        .user_agent("switch-ios-installer")
        .build()
        .map_err(|e| Error::KeyFetch {
            url: url.to_owned(),
            reason: e.to_string(),
        })?;
    let response = client
        .get(parsed)
        .send()
        .and_then(|r| r.error_for_status())
        .map_err(|e| Error::KeyFetch {
            url: url.to_owned(),
            reason: e.to_string(),
        })?;
    let mut pem = Vec::new();
    response
        .take(MAX_PEM_BYTES + 1)
        .read_to_end(&mut pem)
        .map_err(|e| Error::KeyFetch {
            url: url.to_owned(),
            reason: e.to_string(),
        })?;
    if pem.len() as u64 > MAX_PEM_BYTES {
        return Err(Error::KeyFetch {
            url: url.to_owned(),
            reason: format!("response exceeds {MAX_PEM_BYTES} bytes"),
        });
    }
    Ok(pem)
}

fn decode_pem_private_key(pem: &[u8]) -> Result<Vec<u8>> {
    let text = std::str::from_utf8(pem).map_err(|_| Error::KeyUnwrap("private key PEM is not UTF-8".into()))?;
    for label in ["PRIVATE KEY", "EC PRIVATE KEY"] {
        let begin = format!("-----BEGIN {label}-----");
        let end = format!("-----END {label}-----");
        if let Some(after_begin) = text.find(&begin).map(|i| &text[i + begin.len()..]) {
            let Some(body_end) = after_begin.find(&end) else {
                return Err(Error::KeyUnwrap(format!("unterminated PEM block {begin}")));
            };
            let body = &after_begin[..body_end];
            if body.contains(':') {
                return Err(Error::KeyUnwrap(
                    "encrypted or headered PEM private keys are unsupported".into(),
                ));
            }
            let compact: String = body.chars().filter(|c| !c.is_ascii_whitespace()).collect();
            if compact.is_empty() {
                return Err(Error::KeyUnwrap("empty PEM private key body".into()));
            }
            return STANDARD
                .decode(compact.as_bytes())
                .map_err(|e| Error::KeyUnwrap(format!("PEM base64: {e}")));
        }
    }
    Err(Error::KeyUnwrap("no supported PEM private key block found".into()))
}

fn labeled_extract(suite_id: &[u8], salt: &[u8], label: &[u8], ikm: &[u8]) -> ([u8; 32], Hkdf<Sha256>) {
    let mut labeled = Vec::with_capacity(HPKE_VERSION.len() + suite_id.len() + label.len() + ikm.len());
    labeled.extend_from_slice(HPKE_VERSION);
    labeled.extend_from_slice(suite_id);
    labeled.extend_from_slice(label);
    labeled.extend_from_slice(ikm);
    let salt_opt = (!salt.is_empty()).then_some(salt);
    let (prk, hk) = Hkdf::<Sha256>::extract(salt_opt, &labeled);
    let mut prk_bytes = [0u8; 32];
    prk_bytes.copy_from_slice(&prk);
    (prk_bytes, hk)
}

fn labeled_expand<const L: usize>(hk: &Hkdf<Sha256>, suite_id: &[u8], label: &[u8], info: &[u8]) -> [u8; L] {
    let mut labeled = Vec::with_capacity(2 + HPKE_VERSION.len() + suite_id.len() + label.len() + info.len());
    labeled.extend_from_slice(&(L as u16).to_be_bytes());
    labeled.extend_from_slice(HPKE_VERSION);
    labeled.extend_from_slice(suite_id);
    labeled.extend_from_slice(label);
    labeled.extend_from_slice(info);
    let mut out = [0u8; L];
    hk.expand(&labeled, &mut out)
        .expect("RFC 9180 HPKE expand length fits HKDF-SHA256");
    out
}

pub fn unwrap_key(auth: &AuthData, private_key_pem: &[u8]) -> Result<[u8; 32]> {
    let der = decode_pem_private_key(private_key_pem)?;
    let priv_key = PrivateKey::from_private_key_der(&ECDH_P256, &der)
        .map_err(|_| Error::KeyUnwrap("invalid P-256 private key DER".into()))?;
    let pk_r = priv_key
        .compute_public_key()
        .map_err(|_| Error::KeyUnwrap("failed to derive P-256 public key".into()))?;
    let pk_r_bytes = pk_r.as_ref();
    if pk_r_bytes.len() != P256_UNCOMPRESSED_LEN {
        return Err(Error::KeyUnwrap(format!(
            "unexpected P-256 public key length {}",
            pk_r_bytes.len()
        )));
    }

    let dh = agreement::agree(
        &priv_key,
        UnparsedPublicKey::new(&ECDH_P256, &auth.enc_request),
        Error::KeyUnwrap("P-256 ECDH agreement failed".into()),
        |secret| Ok(secret.to_vec()),
    )?;

    let mut kem_context = [0u8; P256_UNCOMPRESSED_LEN * 2];
    kem_context[..P256_UNCOMPRESSED_LEN].copy_from_slice(&auth.enc_request);
    kem_context[P256_UNCOMPRESSED_LEN..].copy_from_slice(pk_r_bytes);

    let (_, eae_prk) = labeled_extract(KEM_SUITE_ID, b"", b"eae_prk", &dh);
    let shared_secret: [u8; 32] = labeled_expand(&eae_prk, KEM_SUITE_ID, b"shared_secret", &kem_context);

    let (psk_id_hash, _) = labeled_extract(HPKE_SUITE_ID, b"", b"psk_id_hash", b"");
    let (info_hash, _) = labeled_extract(HPKE_SUITE_ID, b"", b"info_hash", b"");
    let mut key_schedule_context = [0u8; 1 + 32 + 32];
    key_schedule_context[0] = MODE_BASE;
    key_schedule_context[1..33].copy_from_slice(&psk_id_hash);
    key_schedule_context[33..65].copy_from_slice(&info_hash);

    let (_, secret) = labeled_extract(HPKE_SUITE_ID, &shared_secret, b"secret", b"");
    let aead_key: [u8; 32] = labeled_expand(&secret, HPKE_SUITE_ID, b"key", &key_schedule_context);
    let base_nonce: [u8; 12] = labeled_expand(&secret, HPKE_SUITE_ID, b"base_nonce", &key_schedule_context);

    let unbound =
        UnboundKey::new(&AES_256_GCM, &aead_key).map_err(|_| Error::KeyUnwrap("invalid AES-256-GCM key".into()))?;
    let opening = LessSafeKey::new(unbound);
    let mut buf = auth.wrapped_key.clone();
    let plain = opening
        .open_in_place(Nonce::assume_unique_for_key(base_nonce), Aad::empty(), &mut buf)
        .map_err(|_| Error::KeyUnwrap("HPKE AES-256-GCM decryption failed".into()))?;
    plain
        .try_into()
        .map_err(|_| Error::KeyUnwrap(format!("unwrapped key is {} bytes, expected 32", plain.len())))
}

pub fn unwrap_member_key_with<R, F>(archive: &mut R, mut fetch: F) -> Result<[u8; 32]>
where
    R: Read + Seek,
    F: FnMut(&str) -> Result<Vec<u8>>,
{
    let auth = read_auth_data(archive)?;
    let pem = fetch(&auth.fcs_key_url)?;
    unwrap_key(&auth, &pem)
}

pub fn unwrap_member_key<R: Read + Seek>(archive: &mut R) -> Result<[u8; 32]> {
    unwrap_member_key_with(archive, fetch_fcs_key_pem)
}

#[cfg(test)]
pub(crate) fn hpke_seal_for_test(
    recipient_pk_r: &[u8],
    plaintext: &[u8],
) -> Result<([u8; P256_UNCOMPRESSED_LEN], Vec<u8>)> {
    let eph = PrivateKey::generate(&ECDH_P256).map_err(|_| Error::KeyUnwrap("ephemeral P-256 gen failed".into()))?;
    let enc_pk = eph
        .compute_public_key()
        .map_err(|_| Error::KeyUnwrap("ephemeral P-256 pubkey failed".into()))?;
    let enc: [u8; P256_UNCOMPRESSED_LEN] = enc_pk
        .as_ref()
        .try_into()
        .map_err(|_| Error::KeyUnwrap("unexpected ephemeral pubkey len".into()))?;
    let dh = agreement::agree(
        &eph,
        UnparsedPublicKey::new(&ECDH_P256, recipient_pk_r),
        Error::KeyUnwrap("test ECDH failed".into()),
        |secret| Ok(secret.to_vec()),
    )?;
    let mut kem_context = [0u8; P256_UNCOMPRESSED_LEN * 2];
    kem_context[..P256_UNCOMPRESSED_LEN].copy_from_slice(&enc);
    kem_context[P256_UNCOMPRESSED_LEN..].copy_from_slice(recipient_pk_r);

    let (_, eae_prk) = labeled_extract(KEM_SUITE_ID, b"", b"eae_prk", &dh);
    let shared_secret: [u8; 32] = labeled_expand(&eae_prk, KEM_SUITE_ID, b"shared_secret", &kem_context);

    let (psk_id_hash, _) = labeled_extract(HPKE_SUITE_ID, b"", b"psk_id_hash", b"");
    let (info_hash, _) = labeled_extract(HPKE_SUITE_ID, b"", b"info_hash", b"");
    let mut key_schedule_context = [0u8; 1 + 32 + 32];
    key_schedule_context[0] = MODE_BASE;
    key_schedule_context[1..33].copy_from_slice(&psk_id_hash);
    key_schedule_context[33..65].copy_from_slice(&info_hash);

    let (_, secret) = labeled_extract(HPKE_SUITE_ID, &shared_secret, b"secret", b"");
    let aead_key: [u8; 32] = labeled_expand(&secret, HPKE_SUITE_ID, b"key", &key_schedule_context);
    let base_nonce: [u8; 12] = labeled_expand(&secret, HPKE_SUITE_ID, b"base_nonce", &key_schedule_context);

    let unbound = UnboundKey::new(&AES_256_GCM, &aead_key).unwrap();
    let sealing = LessSafeKey::new(unbound);
    let mut buf = plaintext.to_vec();
    sealing
        .seal_in_place_append_tag(Nonce::assume_unique_for_key(base_nonce), Aad::empty(), &mut buf)
        .map_err(|_| Error::KeyUnwrap("test seal failed".into()))?;
    Ok((enc, buf))
}

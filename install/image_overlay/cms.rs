// SPDX-License-Identifier: GPL-3.0-or-later
//! Pure-Rust RFC 5652 CMS SignedData envelope decoder for `embedded.mobileprovision` validation.

use std::io::Cursor;

use plist::Value;

use crate::Error;

const OID_SIGNED_DATA: &[u8] = &[0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x02];
const OID_DATA: &[u8] = &[0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x01];

const TAG_INTEGER: u8 = 0x02;
const TAG_OCTET_STRING: u8 = 0x04;
const TAG_CONSTRUCTED_OCTET_STRING: u8 = 0x24;
const TAG_OID: u8 = 0x06;
const TAG_SEQUENCE: u8 = 0x30;
const TAG_SET: u8 = 0x31;
const TAG_CONTEXT_0_CONSTRUCTED: u8 = 0xa0;
const TAG_CONTEXT_1_CONSTRUCTED: u8 = 0xa1;

const MAX_DEPTH: usize = 64;

#[derive(Clone, Copy)]
struct Tlv<'a> {
    tag: u8,
    high_tag: bool,
    body: &'a [u8],
}

fn parse_tlv(input: &[u8], depth: usize) -> Result<(Tlv<'_>, &[u8]), Error> {
    if depth > MAX_DEPTH || input.len() < 2 {
        return Err(Error::Profile("invalid ASN.1 TLV".into()));
    }
    let tag = input[0];
    let constructed = (tag & 0x20) != 0;
    let high_tag = (tag & 0x1f) == 0x1f;
    let mut pos = 1;
    if high_tag {
        let mut count = 0;
        loop {
            let b = *input
                .get(pos)
                .ok_or_else(|| Error::Profile("truncated ASN.1 tag".into()))?;
            pos += 1;
            count += 1;
            if count > 4 {
                return Err(Error::Profile("ASN.1 tag too long".into()));
            }
            if b & 0x80 == 0 {
                break;
            }
        }
    }
    let len_byte = *input
        .get(pos)
        .ok_or_else(|| Error::Profile("truncated ASN.1 length".into()))?;
    pos += 1;

    if len_byte == 0x80 {
        if !constructed {
            return Err(Error::Profile("indefinite length on primitive ASN.1 value".into()));
        }
        let body_start = pos;
        let mut scan = &input[body_start..];
        loop {
            if scan.len() < 2 {
                return Err(Error::Profile("unterminated indefinite ASN.1 value".into()));
            }
            if scan[0] == 0x00 && scan[1] == 0x00 {
                let body_len = input.len() - body_start - scan.len();
                let body = &input[body_start..body_start + body_len];
                return Ok((Tlv { tag, high_tag, body }, &scan[2..]));
            }
            let (_, next) = parse_tlv(scan, depth + 1)?;
            scan = next;
        }
    }

    let len = if len_byte < 0x80 {
        len_byte as usize
    } else {
        let n = (len_byte & 0x7f) as usize;
        if n == 0 || n > 4 {
            return Err(Error::Profile("unsupported ASN.1 length".into()));
        }
        let bytes = input
            .get(pos..pos + n)
            .ok_or_else(|| Error::Profile("truncated ASN.1 length bytes".into()))?;
        pos += n;
        let mut v: usize = 0;
        for &b in bytes {
            v = (v << 8) | b as usize;
        }
        v
    };
    let end = pos
        .checked_add(len)
        .filter(|&e| e <= input.len())
        .ok_or_else(|| Error::Profile("ASN.1 length past end of input".into()))?;
    Ok((
        Tlv {
            tag,
            high_tag,
            body: &input[pos..end],
        },
        &input[end..],
    ))
}

fn expect_tag<'a>(input: &'a [u8], tag: u8, depth: usize) -> Result<(&'a [u8], &'a [u8]), Error> {
    let (tlv, rest) = parse_tlv(input, depth)?;
    if tlv.high_tag || tlv.tag != tag {
        return Err(Error::Profile(format!(
            "unexpected ASN.1 tag 0x{:02x}, expected 0x{tag:02x}",
            tlv.tag
        )));
    }
    Ok((tlv.body, rest))
}

fn collect_octet_string(input: &[u8], depth: usize, out: &mut Vec<u8>) -> Result<(), Error> {
    if depth > MAX_DEPTH {
        return Err(Error::Profile("ASN.1 nesting too deep".into()));
    }
    let (tlv, rest) = parse_tlv(input, depth)?;
    if !rest.is_empty() || tlv.high_tag {
        return Err(Error::Profile("unexpected trailing data in eContent".into()));
    }
    match tlv.tag {
        TAG_OCTET_STRING => {
            out.extend_from_slice(tlv.body);
            Ok(())
        }
        TAG_CONSTRUCTED_OCTET_STRING => {
            let mut cursor = tlv.body;
            while !cursor.is_empty() {
                let (chunk, next) = parse_tlv(cursor, depth + 1)?;
                if chunk.high_tag {
                    return Err(Error::Profile("invalid constructed octet string".into()));
                }
                match chunk.tag {
                    TAG_OCTET_STRING => out.extend_from_slice(chunk.body),
                    TAG_CONSTRUCTED_OCTET_STRING => {
                        let consumed = cursor.len() - next.len();
                        collect_octet_string(&cursor[..consumed], depth + 1, out)?;
                    }
                    _ => return Err(Error::Profile("non-octet chunk inside octet string".into())),
                }
                cursor = next;
            }
            Ok(())
        }
        _ => Err(Error::Profile("CMS eContent is not an OCTET STRING".into())),
    }
}

fn validate_well_formed_children(mut input: &[u8], depth: usize) -> Result<(), Error> {
    while !input.is_empty() {
        let (_, rest) = parse_tlv(input, depth)?;
        input = rest;
    }
    Ok(())
}

/// Decodes a DER/BER CMS `SignedData` `ContentInfo` envelope and returns its encapsulated `id-data` payload.
pub fn extract_signed_data(input: &[u8]) -> Result<Vec<u8>, Error> {
    let (content_info, rest) = expect_tag(input, TAG_SEQUENCE, 0)?;
    if !rest.is_empty() {
        return Err(Error::Profile("trailing bytes after CMS ContentInfo".into()));
    }
    let (oid, rest) = expect_tag(content_info, TAG_OID, 1)?;
    if oid != OID_SIGNED_DATA {
        return Err(Error::Profile("CMS ContentInfo is not SignedData".into()));
    }
    let (explicit_content, rest) = expect_tag(rest, TAG_CONTEXT_0_CONSTRUCTED, 1)?;
    if !rest.is_empty() {
        return Err(Error::Profile("trailing fields in CMS ContentInfo".into()));
    }
    let (signed_data, rest) = expect_tag(explicit_content, TAG_SEQUENCE, 2)?;
    if !rest.is_empty() {
        return Err(Error::Profile("trailing bytes after SignedData".into()));
    }

    let (version, rest) = expect_tag(signed_data, TAG_INTEGER, 3)?;
    if version.is_empty() {
        return Err(Error::Profile("empty CMSVersion".into()));
    }
    let (digest_algs, rest) = expect_tag(rest, TAG_SET, 3)?;
    validate_well_formed_children(digest_algs, 4)?;

    let (encap_info, mut rest) = expect_tag(rest, TAG_SEQUENCE, 3)?;
    let (e_oid, encap_rest) = expect_tag(encap_info, TAG_OID, 4)?;
    if e_oid != OID_DATA {
        return Err(Error::Profile("CMS EncapsulatedContentInfo is not id-data".into()));
    }
    let (e_content, encap_tail) = expect_tag(encap_rest, TAG_CONTEXT_0_CONSTRUCTED, 4)?;
    if !encap_tail.is_empty() {
        return Err(Error::Profile("trailing fields in EncapsulatedContentInfo".into()));
    }
    let mut payload = Vec::new();
    collect_octet_string(e_content, 5, &mut payload)?;

    let mut next_tlv = parse_tlv(rest, 3)?;
    if !next_tlv.0.high_tag && next_tlv.0.tag == TAG_CONTEXT_0_CONSTRUCTED {
        validate_well_formed_children(next_tlv.0.body, 4)?;
        rest = next_tlv.1;
        next_tlv = parse_tlv(rest, 3)?;
    }
    if !next_tlv.0.high_tag && next_tlv.0.tag == TAG_CONTEXT_1_CONSTRUCTED {
        validate_well_formed_children(next_tlv.0.body, 4)?;
        rest = next_tlv.1;
        next_tlv = parse_tlv(rest, 3)?;
    }
    let (signer_infos, tail) = next_tlv;
    if signer_infos.high_tag || signer_infos.tag != TAG_SET || !tail.is_empty() {
        return Err(Error::Profile("SignedData missing valid signerInfos SET".into()));
    }
    validate_well_formed_children(signer_infos.body, 4)?;

    Ok(payload)
}

/// Validates an app's `embedded.mobileprovision` matches `staged_apps._check_embedded_profile`.
pub fn verify_embedded_profile(app_name: &str, bytes: &[u8]) -> Result<(), Error> {
    let payload = extract_signed_data(bytes).map_err(|_| {
        Error::Profile(format!(
            "{app_name}: embedded provisioning profile is not a decodable CMS profile"
        ))
    })?;
    let value = Value::from_reader(Cursor::new(&payload)).map_err(|_| {
        Error::Profile(format!(
            "{app_name}: embedded provisioning profile is not a decodable CMS profile"
        ))
    })?;
    let Some(dict) = value.as_dictionary() else {
        return Err(Error::Profile(format!(
            "{app_name}: embedded provisioning profile has no entitlements dictionary"
        )));
    };
    if !matches!(dict.get("Entitlements"), Some(Value::Dictionary(_))) {
        return Err(Error::Profile(format!(
            "{app_name}: embedded provisioning profile has no entitlements dictionary"
        )));
    }
    Ok(())
}

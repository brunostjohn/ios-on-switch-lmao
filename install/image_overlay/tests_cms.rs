// SPDX-License-Identifier: GPL-3.0-or-later
use crate::cms::{extract_signed_data, verify_embedded_profile};

fn der_tlv(tag: u8, body: &[u8]) -> Vec<u8> {
    let mut out = vec![tag];
    if body.len() < 0x80 {
        out.push(body.len() as u8);
    } else {
        let bytes = (body.len() as u64).to_be_bytes();
        let skip = bytes.iter().take_while(|&&b| b == 0).count();
        out.push(0x80 | (8 - skip) as u8);
        out.extend_from_slice(&bytes[skip..]);
    }
    out.extend_from_slice(body);
    out
}

fn ber_indefinite(tag: u8, body: &[u8]) -> Vec<u8> {
    let mut out = vec![tag, 0x80];
    out.extend_from_slice(body);
    out.extend_from_slice(&[0x00, 0x00]);
    out
}

pub(crate) fn wrap_cms_der(payload: &[u8]) -> Vec<u8> {
    let oid_signed_data = der_tlv(0x06, &[0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x02]);
    let oid_data = der_tlv(0x06, &[0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x01]);
    let version = der_tlv(0x02, &[0x01]);
    let digest_algs = der_tlv(0x31, &[]);
    let e_content = der_tlv(0xa0, &der_tlv(0x04, payload));
    let mut encap_body = Vec::new();
    encap_body.extend(oid_data);
    encap_body.extend(e_content);
    let encap_info = der_tlv(0x30, &encap_body);
    let certs = der_tlv(0xa0, &der_tlv(0x30, &[0x05, 0x00]));
    let signer_infos = der_tlv(0x31, &der_tlv(0x30, &der_tlv(0x02, &[0x01])));

    let mut sd_body = Vec::new();
    sd_body.extend(version);
    sd_body.extend(digest_algs);
    sd_body.extend(encap_info);
    sd_body.extend(certs);
    sd_body.extend(signer_infos);
    let signed_data = der_tlv(0x30, &sd_body);

    let mut ci_body = Vec::new();
    ci_body.extend(oid_signed_data);
    ci_body.extend(der_tlv(0xa0, &signed_data));
    der_tlv(0x30, &ci_body)
}

fn sample_profile_plist() -> Vec<u8> {
    br#"<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Name</key>
    <string>Switch Installer Profile</string>
    <key>Entitlements</key>
    <dict>
        <key>application-identifier</key>
        <string>ABCDE12345.com.brunostjohn.switch.installer</string>
    </dict>
</dict>
</plist>
"#
    .to_vec()
}

#[test]
fn valid_der_signed_data_and_profile_succeed() {
    let plist = sample_profile_plist();
    let cms = wrap_cms_der(&plist);
    assert_eq!(extract_signed_data(&cms).unwrap(), plist);
    verify_embedded_profile("SwitchInstaller.app", &cms).unwrap();
}

#[test]
fn valid_ber_indefinite_constructed_octet_string_succeeds() {
    let plist = sample_profile_plist();
    let mid = plist.len() / 2;
    let mut chunks = der_tlv(0x04, &plist[..mid]);
    chunks.extend(der_tlv(0x04, &plist[mid..]));
    let octets = ber_indefinite(0x24, &chunks);

    let oid_signed_data = der_tlv(0x06, &[0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x02]);
    let oid_data = der_tlv(0x06, &[0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x01]);
    let mut encap_body = Vec::new();
    encap_body.extend(oid_data);
    encap_body.extend(ber_indefinite(0xa0, &octets));
    let encap_info = ber_indefinite(0x30, &encap_body);

    let mut sd_body = Vec::new();
    sd_body.extend(der_tlv(0x02, &[0x01]));
    sd_body.extend(der_tlv(0x31, &[]));
    sd_body.extend(encap_info);
    sd_body.extend(der_tlv(0x31, &[]));
    let signed_data = ber_indefinite(0x30, &sd_body);

    let mut ci_body = Vec::new();
    ci_body.extend(oid_signed_data);
    ci_body.extend(ber_indefinite(0xa0, &signed_data));
    let cms = ber_indefinite(0x30, &ci_body);

    assert_eq!(extract_signed_data(&cms).unwrap(), plist);
    verify_embedded_profile("SwitchInstaller.app", &cms).unwrap();
}

#[test]
fn unsigned_plain_xml_plist_is_refused() {
    let plist = sample_profile_plist();
    let err = verify_embedded_profile("SwitchInstaller.app", &plist).unwrap_err();
    assert!(
        err.to_string().contains("not a decodable CMS profile"),
        "unexpected error: {err}"
    );
}

#[test]
fn profile_without_entitlements_dict_is_refused() {
    let no_ent = br#"<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict><key>Name</key><string>x</string></dict></plist>"#;
    let err = verify_embedded_profile("SwitchInstaller.app", &wrap_cms_der(no_ent)).unwrap_err();
    assert!(
        err.to_string().contains("no entitlements dictionary"),
        "unexpected error: {err}"
    );

    let bad_ent = br#"<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict><key>Entitlements</key><string>not-a-dict</string></dict></plist>"#;
    let err = verify_embedded_profile("SwitchInstaller.app", &wrap_cms_der(bad_ent)).unwrap_err();
    assert!(
        err.to_string().contains("no entitlements dictionary"),
        "unexpected error: {err}"
    );
}

#[test]
fn truncated_or_wrong_oid_cms_is_refused() {
    let mut cms = wrap_cms_der(&sample_profile_plist());
    cms.truncate(cms.len() - 5);
    assert!(extract_signed_data(&cms).is_err());

    let mut with_trailing = wrap_cms_der(&sample_profile_plist());
    with_trailing.push(0x00);
    assert!(extract_signed_data(&with_trailing).is_err());

    let wrong_oid = der_tlv(0x06, &[0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x01]);
    let bad_ci = der_tlv(0x30, &wrong_oid);
    assert!(extract_signed_data(&bad_ci).is_err());
}

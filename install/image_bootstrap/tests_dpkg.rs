// SPDX-License-Identifier: GPL-3.0-or-later
use crate::{format_python_json, parse_control, rewrite_status};

#[test]
fn dpkg_continuation_lines_and_upsert_firmware() {
    let input = concat!(
        "Package: org.coolstar.sileo\n",
        "Status: install ok installed\n",
        "Version: 2.5.1\n",
        "Description: Sileo package manager\n",
        " A modern APT frontend.\n",
        " .\n",
        " Second paragraph.\n\n",
        "Package: ellekit\n",
        "Status: hold ok installed\n",
        "Version: 1.1\n\n",
        "Package: uikittools\n",
        "Status: install ok installed\n",
        "Version: 2.1\n\n",
        "Package: launchctl\n",
        "Status: install ok installed\n",
        "Version: 1:1.1.1\n",
    );

    let out = rewrite_status(input, "17.7.11").unwrap();
    let expected = concat!(
        "Package: org.coolstar.sileo\n",
        "Status: hold ok installed\n",
        "Version: 2.5.1\n",
        "Description: Sileo package manager\n",
        " A modern APT frontend.\n",
        " .\n",
        " Second paragraph.\n\n",
        "Package: ellekit\n",
        "Status: hold ok installed\n",
        "Version: 1.1\n\n",
        "Package: uikittools\n",
        "Status: hold ok installed\n",
        "Version: 2.1\n\n",
        "Package: launchctl\n",
        "Status: hold ok installed\n",
        "Version: 1:1.1.1\n\n",
        "Package: firmware\n",
        "Status: hold ok installed\n",
        "Priority: required\n",
        "Section: System\n",
        "Essential: yes\n",
        "Architecture: iphoneos-arm64\n",
        "Version: 17.7.11\n",
        "Description: iOS firmware version\n",
    );
    assert_eq!(out, expected);
}

#[test]
fn dpkg_refuses_malformed_duplicate_missing_or_unconfigured() {
    assert!(parse_control("NoColonHere\n").is_err());
    assert!(parse_control(": empty key\n").is_err());
    assert!(parse_control(" leading space before any key\n").is_err());
    assert!(parse_control("Package: a\nPackage: a\n").is_err());

    let dup = format!(
        "{}\nPackage: ellekit\nStatus: install ok installed\n",
        crate::tests::SAMPLE_STATUS
    );
    assert!(rewrite_status(&dup, "17.7.11").is_err());

    let missing = "Package: ellekit\nStatus: install ok installed\n";
    assert!(rewrite_status(missing, "17.7.11").is_err());

    let unpacked = crate::tests::SAMPLE_STATUS.replace("install ok installed", "install ok unpacked");
    assert!(rewrite_status(&unpacked, "17.7.11").is_err());
}

#[test]
fn python_json_formatting_escapes_and_sorts_identically() {
    let val = serde_json::json!({
        "b": 1,
        "a": "caf\u{00e9}\u{007f}\u{0008}\u{000c}\n\r\t\u{0001}\u{1f600}",
        "c": [],
        "d": {}
    });
    assert_eq!(
        format_python_json(&val),
        "{\"a\": \"caf\\u00e9\\u007f\\b\\f\\n\\r\\t\\u0001\\ud83d\\ude00\", \"b\": 1, \"c\": [], \"d\": {}}\n"
    );
}

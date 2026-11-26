// SPDX-License-Identifier: GPL-3.0-or-later
use crate::info::{Value, serialize, unserialize, unserialize_plist};
use crate::macho::backed_offset;
use crate::v17::pc_relative_target;
use crate::{Error, load_sites};

fn refused<T: std::fmt::Debug>(result: Result<T, Error>) -> String {
    match result {
        Err(Error::Refused(message)) => message,
        other => panic!("expected a refusal, got {other:?}"),
    }
}

fn write(value: &Value) -> String {
    String::from_utf8(serialize(value, &|v| Ok(v as u64)).unwrap()).unwrap()
}

/// What native_prelink.unserialize_xml, then emit_prelink.to_xml with ElementTree, make of the same document
/// (Python 3.13).
#[test]
fn prelink_info_round_trip_matches_python() {
    let document = "<dict><key>A</key><integer ID=\"0\" size=\"64\">0x10</integer>\
        <key>B</key><!-- \u{1f926} --><integer IDREF=\"0\"/>\
        <key>C</key><string ID=\"1\">x &amp; &lt;y&gt; \u{a9}</string>\
        <key>D</key><string IDREF=\"1\"/>\
        <key>E</key><array><true/><false/><data>AAEC</data><data></data><string></string><dict/><array/></array>\
        <key>F</key><integer>12</integer><key>G</key><dict><key></key><string>e</string></dict>\
        <key>H</key><data>\n  QUJD\n  RA==\n</data><key>I</key><integer>0x0</integer>\
        </dict>\0trailing";
    let python = "<dict><key>A</key><integer size=\"64\">0x10</integer><key>B</key><integer size=\"64\">0x10</integer>\
        <key>C</key><string>x &amp; &lt;y&gt; \u{a9}</string><key>D</key><string>x &amp; &lt;y&gt; \u{a9}</string>\
        <key>E</key><array><true/><false/><data>AAEC</data><data/><string/><dict/><array/></array>\
        <key>F</key><integer size=\"64\">0xc</integer><key>G</key><dict><key/><string>e</string></dict>\
        <key>H</key><data>QUJDRA==</data><key>I</key><integer size=\"64\">0x0</integer></dict>";
    assert_eq!(write(&unserialize(document.as_bytes()).unwrap()), python);
}

#[test]
fn prelink_info_refusals() {
    let cases = [
        ("<dict><key>a</key></dict>", "Odd XML dictionary"),
        (
            "<dict><key>a</key><true/><key>a</key><false/></dict>",
            "Duplicate XML dictionary key",
        ),
        ("<dict><key>a</key><integer IDREF=\"9\"/></dict>", "IDREF 9"),
        ("<dict><key>a</key><dict ID=\"0\"/></dict>", "shares a container"),
        (
            "<dict><key>a</key><real>1.0</real></dict>",
            "Unsupported prelink XML element real",
        ),
        (
            "<dict><key>a</key><integer>010</integer></dict>",
            "Prelink info integer",
        ),
        ("<dict><key>a</key><data>QQ</data></dict>", "not base64"),
        ("<dict><integer>1</integer><true/></dict>", "not a string"),
        ("<dict/><dict/>", "not one element"),
    ];
    for (document, message) in cases {
        let found = refused(unserialize(document.as_bytes()));
        assert!(found.contains(message), "{document}: {found}");
    }
}

#[test]
fn integers_outside_the_writer_policy_are_refused() {
    let value = unserialize(b"<array><integer>-1</integer></array>").unwrap();
    let policy = |v: i128| u64::try_from(v).map_err(|_| Error::Refused("out of range".into()));
    assert!(matches!(serialize(&value, &policy), Err(Error::Refused(_))));
}

#[test]
fn pc_relative_encodings() {
    let pc = 0xffff_fff0_0700_0004_u64;
    let pc_i = i128::from(pc);
    let cases: [(u32, Option<(i128, bool)>); 8] = [
        (0x9000_0010, Some(((pc_i & !4095), true)) /* adrp x16, 0 */),
        (
            0xb000_0010,
            Some(((pc_i & !4095) + 0x1000, true)), /* adrp x16, +1 page */
        ),
        (0x1000_0050, Some((pc_i + 8, false)) /* adr x16, +8 */),
        (0x97ff_ffff, Some((pc_i - 4, false)) /* bl -4 */),
        (0x5400_0040, Some((pc_i + 8, false)) /* b.eq +8 */),
        (0xb400_0040, Some((pc_i + 8, false)) /* cbz x0, +8 */),
        (0x3600_0040, Some((pc_i + 8, false)) /* tbz w0, #0, +8 */),
        (0xd503_201f, None /* nop */),
    ];
    for (word, expected) in cases {
        assert_eq!(pc_relative_target(word, pc), expected, "{word:08x}");
    }
}

#[test]
fn site_tables() {
    let table = br#"{"kind": "K", "bundle": "b", "doc": "", "sites": [
        {"at": "0xfffffff005ebfd54", "from": "710c011f", "to": "7104011f", "why": "w"}]}"#;
    let sites = load_sites(&[table], "K").unwrap();
    assert_eq!(
        (sites[0].at, sites[0].original, sites[0].replacement),
        (0xffff_fff0_05eb_fd54, 0x710c_011f, 0x7104_011f)
    );
    assert!(refused(load_sites(&[table], "L")).contains("sites for K, not L"));
    assert!(refused(load_sites(&[table, table], "K")).contains("declared twice"));
}

#[test]
fn backed_offsets_need_exactly_one_segment() {
    let segments = [(0x1000, 0x100, 0x0), (0x2000, 0x100, 0x100)];
    assert_eq!(backed_offset(segments, 0x2010, 8, 0x200).unwrap(), 0x110);
    assert!(backed_offset(segments, 0x10f8, 16, 0x200).is_err());
    assert!(backed_offset([(0x0, 0x100, 0x0), (0x0, 0x100, 0x0)], 0x10, 8, 0x200).is_err());
    assert!(backed_offset(segments, 0x2010, 8, 0x110).is_err());
}

#[test]
fn fileset_prelink_info_is_a_plist() {
    let document = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \
        \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n<dict><key>A</key>\
        <integer>-1</integer></dict>\n</plist>\n\0";
    assert_eq!(
        write(&unserialize_plist(document.as_bytes()).unwrap()),
        "<dict><key>A</key><integer size=\"64\">0xffffffffffffffff</integer></dict>"
    );
    assert!(refused(unserialize(document.as_bytes())).contains("not one element"));
    let cases = [
        (
            "<!DOCTYPE plist [<!ENTITY a \"b\">]><plist><dict/></plist>",
            "internal subset",
        ),
        ("<plist><dict/><dict/></plist>", "not one element"),
        ("<plist/>", "not one element"),
    ];
    for (document, message) in cases {
        let found = refused(unserialize_plist(document.as_bytes()));
        assert!(found.contains(message), "{document}: {found}");
    }
}

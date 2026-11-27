// SPDX-License-Identifier: GPL-3.0-or-later
//! switch_sign against /usr/bin/codesign itself, on files made here: every signature must be the same bytes.

mod fixture;

use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use fixture::{ARM64, Fixture, Version, X86_64};
use sha2::{Digest, Sha256};
use switch_sign::{Detached, Embedded, Error, Metadata, RUNTIME, Signed, sign_detached, sign_macho};

const CATALOG_ID: &str = "com.brunostjohn.switch.launchd-catalog";

const MIXED: &str = r#"<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>zeta</key>    <true/>
  <key>b</key><string>bee</string>
  <key>ab</key><false/>
  <key>a</key><integer>-5</integer>
  <key>B</key><integer>300</integer>
  <key>a.b.c</key><array><string>x</string><integer>0</integer><true/><dict><key>k</key><string>v</string></dict></array>
  <key>nested</key><dict><key>zz</key><string>1</string><key>y</key><string>22</string></dict>
  <key>big</key><integer>9223372036854775807</integer>
  <key>min</key><integer>-9223372036854775808</integer>
  <key>wraps</key><integer>18446744073709551615</integer>
  <key>uni</key><string>café &amp; ü 😀</string>
  <key>emptyd</key><dict/>
  <key>emptya</key><array/>
  <key>long</key><string>0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789</string>
</dict>
</plist>
"#;

const SMALL: &str = r#"<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>com.apple.private.security.storage.AppBundles</key>
	<true/>
	<key>get-task-allow</key>
	<true/>
</dict>
</plist>
"#;

const EXEC_FLAGS: &str = r#"<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0">
<dict>
	<key>dynamic-codesigning</key><true/>
	<key>com.apple.private.skip-library-validation</key><true/>
	<key>com.apple.private.cs.debugger</key><false/>
	<key>run-unsigned-code</key><string>yes</string>
	<key>com.apple.private.amfi.can-load-cdhash</key><true/>
	<key>com.apple.private.amfi.can-execute-cdhash</key><true/>
</dict>
</plist>
"#;

struct Scratch(tempfile::TempDir);

impl Scratch {
    fn new() -> Self {
        Scratch(tempfile::tempdir().expect("a scratch directory"))
    }

    fn write(&self, name: &str, data: &[u8]) -> PathBuf {
        let path = self.0.path().join(name);
        fs::write(&path, data).expect("writing a test file");
        path
    }
}

fn codesign(args: &[&str]) -> String {
    let out = Command::new("/usr/bin/codesign")
        .args(args)
        .output()
        .expect("running codesign");
    assert!(
        out.status.success(),
        "codesign {args:?}: {}",
        String::from_utf8_lossy(&out.stderr)
    );
    String::from_utf8_lossy(&out.stderr).into_owned()
}

fn cdhash_of(display: &str) -> String {
    display
        .lines()
        .find_map(|l| l.strip_prefix("CDHash="))
        .expect("codesign shows a CDHash")
        .to_owned()
}

fn compare(case: &str, ours: &Signed, theirs: &[u8], display: Option<&str>) {
    let first = ours.bytes.iter().zip(theirs).position(|(a, b)| a != b);
    let first = first.or((ours.bytes.len() != theirs.len()).then(|| ours.bytes.len().min(theirs.len())));
    let verdict = match first {
        None => "equal".to_owned(),
        Some(at) => format!("DIFFER at {at:#x}, {} vs {} bytes", ours.bytes.len(), theirs.len()),
    };
    println!(
        "{case}: codesign {} ours {} cdhash {} ({verdict})",
        hex::encode(Sha256::digest(theirs)),
        hex::encode(Sha256::digest(&ours.bytes)),
        hex::encode(ours.cdhash),
    );
    assert!(first.is_none(), "{case} differs from codesign");
    if let Some(display) = display {
        assert_eq!(hex::encode(ours.cdhash), cdhash_of(display), "{case} CDHash");
    }
}

fn path(p: &Path) -> &str {
    p.to_str().expect("UTF-8 path")
}

/// Signs `input` with codesign (`args`, the entitlements, then the file) and with switch_sign given the same
/// options, and checks they agree. Returns codesign's output.
fn macho(scratch: &Scratch, case: &str, input: &[u8], args: &[&str], entitlements: Option<&str>) -> Vec<u8> {
    let file = scratch.write(case, input);
    let rights = entitlements.map(|xml| scratch.write(&format!("{case}.entitlements"), xml.as_bytes()));
    let mut all: Vec<&str> = args.to_vec();
    if let Some(rights) = &rights {
        all.extend(["--entitlements", path(rights), "--generate-entitlement-der"]);
    }
    all.push(path(&file));
    codesign(&all);
    let theirs = fs::read(&file).expect("reading codesign's output");
    let display = codesign(&["--display", "--verbose=3", path(&file)]);
    let metadata = match args.iter().position(|a| *a == "-i") {
        Some(at) => Metadata::Given {
            identifier: args[at + 1],
            flags: if args.contains(&"runtime") { RUNTIME } else { 0 },
        },
        None => Metadata::Preserve,
    };
    let options = Embedded {
        metadata,
        entitlements: entitlements.map(str::as_bytes),
    };
    let ours = sign_macho(input, &options).unwrap_or_else(|e| panic!("{case}: {e}"));
    compare(case, &ours, &theirs, Some(&display));
    theirs
}

const FRESH: &[&str] = &["-s", "-", "--timestamp=none"];
const RESIGN: &[&str] = &[
    "--force",
    "-s",
    "-",
    "--timestamp=none",
    "--preserve-metadata=identifier,flags,runtime",
];

fn fresh<'a>(identifier: &'a str, runtime: bool) -> Vec<&'a str> {
    let mut args = FRESH.to_vec();
    args.extend(["-i", identifier]);
    if runtime {
        args.extend(["-o", "runtime"]);
    }
    args
}

#[test]
fn detached_generic() {
    let scratch = Scratch::new();
    for size in [0usize, 1, 4095, 4096, 4097, 17_400, 70_001] {
        let mut data: Vec<u8> = (0..size).map(|i| (i * 31 % 251) as u8).collect();
        data[..size.min(8)].copy_from_slice(&b"bplist00"[..size.min(8)]);
        let file = scratch.write(&format!("catalog-{size}.plist"), &data);
        let sig = scratch.0.path().join(format!("catalog-{size}.sig"));
        codesign(&[
            "--sign",
            "-",
            "--identifier",
            CATALOG_ID,
            "--options",
            "runtime",
            "--timestamp=none",
            "--pagesize",
            "4096",
            "--digest-algorithm",
            "sha256",
            "--detached",
            path(&sig),
            path(&file),
        ]);
        let theirs = fs::read(&sig).expect("reading the detached signature");
        // codesign signs an empty file but won't display that signature.
        let display =
            (size > 0).then(|| codesign(&["--display", "--verbose=3", "--detached", path(&sig), path(&file)]));
        let options = Detached {
            identifier: CATALOG_ID,
            flags: RUNTIME,
            page_size: 4096,
        };
        compare(
            &format!("detached {size} B"),
            &sign_detached(&data, &options).unwrap(),
            &theirs,
            display.as_deref(),
        );
    }
}

#[test]
fn resigning_keeps_metadata_and_resizes() {
    let scratch = Scratch::new();
    let input = Fixture::ios(0x8000, 0x123).build();
    let signed = macho(
        &scratch,
        "ios-fresh",
        &input,
        &fresh("com.example.fresh", false),
        Some(MIXED),
    );
    let smaller = macho(&scratch, "ios-resign-smaller", &signed, RESIGN, Some(SMALL));
    macho(&scratch, "ios-resign-larger", &smaller, RESIGN, Some(MIXED));
    macho(&scratch, "ios-resign-none", &smaller, RESIGN, None);
}

#[test]
fn runtime_version() {
    let scratch = Scratch::new();
    let input = Fixture::ios(0xc000, 0x2345).build();
    let signed = macho(
        &scratch,
        "runtime-fresh",
        &input,
        &fresh("com.example.runtime", true),
        None,
    );
    macho(&scratch, "runtime-resign", &signed, RESIGN, Some(SMALL));
}

#[test]
fn exec_segment_flags() {
    let scratch = Scratch::new();
    let input = Fixture::ios(0x8000, 0x200).build();
    macho(
        &scratch,
        "exec-flags",
        &input,
        &fresh("com.example.exec", false),
        Some(EXEC_FLAGS),
    );
}

#[test]
fn page_sizes_and_platforms() {
    let scratch = Scratch::new();
    let base = Fixture::ios(0x8000, 0x123);
    let cases = [
        (
            "macos-x86_64",
            X86_64,
            Version::Build {
                platform: 1,
                min_os: 0x0d_0000,
                sdk: 0x0d_0000,
            },
        ),
        (
            "tvos",
            ARM64,
            Version::Build {
                platform: 3,
                min_os: 0x11_0000,
                sdk: 0x11_0000,
            },
        ),
        (
            "version-min",
            ARM64,
            Version::Min {
                command: 0x25,
                min_os: 0x0c_0000,
                sdk: 0x0c_0000,
            },
        ),
        ("no-platform", ARM64, Version::None),
    ];
    for (case, cpu, version) in cases {
        let input = Fixture {
            cpu,
            version,
            ..base.clone()
        }
        .build();
        let signed = macho(
            &scratch,
            case,
            &input,
            &fresh("com.example.platform", false),
            Some(SMALL),
        );
        macho(&scratch, &format!("{case}-resign"), &signed, RESIGN, None);
    }
}

#[test]
fn layouts() {
    let scratch = Scratch::new();
    let large = Fixture {
        linkedit_vmsize: 0x4_0000,
        ..Fixture::ios(0x4_8000, 0x1235)
    };
    macho(
        &scratch,
        "large",
        &large.build(),
        &fresh("com.example.large", false),
        Some(MIXED),
    );
    let macos = Version::Build {
        platform: 1,
        min_os: 0x0d_0000,
        sdk: 0x0d_0000,
    };
    let large_x86 = Fixture {
        cpu: X86_64,
        version: macos,
        ..large
    };
    macho(
        &scratch,
        "large-x86_64",
        &large_x86.build(),
        &fresh("com.example.large", false),
        Some(SMALL),
    );
    let info = br#"<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict><key>CFBundleIdentifier</key><string>com.example.info</string></dict></plist>
"#;
    let with_info = Fixture {
        info_plist: Some(info.to_vec()),
        ..Fixture::ios(0x8000, 0x123)
    };
    let signed = macho(
        &scratch,
        "info-plist",
        &with_info.build(),
        &fresh("com.example.info", false),
        None,
    );
    macho(&scratch, "info-plist-resign", &signed, RESIGN, Some(SMALL));
    for length in 1..=20 {
        let identifier = "i".repeat(length);
        let input = Fixture::ios(0x8000, 0x123 + length as u64).build();
        macho(
            &scratch,
            &format!("identifier-{length}"),
            &input,
            &fresh(&identifier, length % 2 == 0),
            Some(SMALL),
        );
    }
}

#[test]
fn refusals() {
    let given = |entitlements: Option<&'static str>| Embedded {
        metadata: Metadata::Given {
            identifier: "com.example.refused",
            flags: 0,
        },
        entitlements: entitlements.map(str::as_bytes),
    };
    let old_ios = Fixture {
        version: Version::Build {
            platform: 2,
            min_os: 0x0a_0000,
            sdk: 0x0a_0000,
        },
        ..Fixture::ios(0x8000, 0x123)
    };
    assert!(matches!(
        sign_macho(&old_ios.build(), &given(None)),
        Err(Error::Unsupported(_))
    ));
    let unsigned = Fixture::ios(0x8000, 0x123).build();
    let preserve = Embedded {
        metadata: Metadata::Preserve,
        entitlements: None,
    };
    assert!(matches!(sign_macho(&unsigned, &preserve), Err(Error::Unsupported(_))));
    for value in [
        "<data>AAEC</data>",
        "<real>1.5</real>",
        "<date>2020-01-01T00:00:00Z</date>",
    ] {
        let xml = format!("<?xml version=\"1.0\"?><plist version=\"1.0\"><dict><key>k</key>{value}</dict></plist>");
        let options = Embedded {
            entitlements: Some(xml.as_bytes()),
            ..given(None)
        };
        assert!(
            matches!(sign_macho(&unsigned, &options), Err(Error::Entitlements(_))),
            "{value}"
        );
    }
    let mixed_keys = "<?xml version=\"1.0\"?><plist><dict><key>é</key><true/><key>z</key><true/></dict></plist>";
    let options = Embedded {
        entitlements: Some(mixed_keys.as_bytes()),
        ..given(None)
    };
    assert!(matches!(sign_macho(&unsigned, &options), Err(Error::Entitlements(_))));
    let macho_file = Detached {
        identifier: CATALOG_ID,
        flags: RUNTIME,
        page_size: 4096,
    };
    assert!(matches!(
        sign_detached(&unsigned, &macho_file),
        Err(Error::Unsupported(_))
    ));
}

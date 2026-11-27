// SPDX-License-Identifier: GPL-3.0-or-later
use crate::tree::Node;
use crate::{Error, Finish, Input, TrustModules, finish};

fn prop(name: &str, value: &[u8]) -> Vec<u8> {
    prop_word(name, value.len() as u32, value)
}

fn prop_word(name: &str, word: u32, value: &[u8]) -> Vec<u8> {
    let mut out = [name.as_bytes(), &[0; 32][name.len()..]].concat();
    out.extend_from_slice(&word.to_le_bytes());
    out.extend_from_slice(value);
    out.resize(out.len() + (4 - value.len() % 4) % 4, 0);
    out
}

fn node(props: &[Vec<u8>], children: &[Vec<u8>]) -> Vec<u8> {
    let mut out = (props.len() as u32).to_le_bytes().to_vec();
    out.extend_from_slice(&(children.len() as u32).to_le_bytes());
    out.extend(props.concat());
    out.extend(children.concat());
    out
}

fn named(name: &str, props: &[Vec<u8>], children: &[Vec<u8>]) -> Vec<u8> {
    let props = [&[prop("name", &[name.as_bytes(), b"\0"].concat())][..], props].concat();
    node(&props, children)
}

fn base() -> Vec<u8> {
    let chosen = named(
        "chosen",
        &[prop("root-matching", b"none\0"), prop("random-seed", &[7; 8])],
        &[],
    );
    let product = named(
        "product",
        &[prop("product-name", b"Switch\0")],
        &[named("maps", &[], &[])],
    );
    let filesystems = named("filesystems", &[prop("DoNotUseSEP", &1u32.to_le_bytes())], &[]);
    named(
        "device-tree",
        &[prop("compatible", b"nintendo,nx\0")],
        &[chosen, product, named("arm-io", &[], &[]), filesystems],
    )
}

fn apple_with(arm_io_compatible: &[u8], backlights: usize) -> Vec<u8> {
    let product = named(
        "product",
        &[
            prop("product-name", b"iPad\0"),
            prop("thin-bezel", &1u32.to_le_bytes()),
            prop("not-identity", b"x\0"),
        ],
        &[named("audio", &[prop("acoustic-id", &2003u32.to_le_bytes())], &[])],
    );
    let arm_io = named(
        "arm-io",
        &[prop("compatible", arm_io_compatible), prop("soc-generation", b"H12\0")],
        &[],
    );
    let backlight = named(
        "backlight",
        &[prop("default-whitepoint-type", &1u32.to_le_bytes())],
        &[],
    );
    let mut children = vec![product, arm_io];
    children.extend(std::iter::repeat_n(backlight, backlights));
    named(
        "device-tree",
        &[
            prop("model", b"iPad12,1\0"),
            prop("target-type", b"J181\0"),
            prop("target-sub-type", b"J181AP\0"),
            prop("compatible", b"J181AP\0iPad12,1\0AppleARM\0"),
        ],
        &children,
    )
}

fn apple() -> Vec<u8> {
    apple_with(b"arm-io,t8030\0arm-io\0", 1)
}

fn module(hashes: &[u8]) -> Vec<u8> {
    let mut out = 2u32.to_le_bytes().to_vec();
    out.extend_from_slice(&[0; 16]);
    out.extend_from_slice(&(hashes.len() as u32).to_le_bytes());
    for &h in hashes {
        out.extend_from_slice(&[h; 20]);
        out.extend_from_slice(&[2, 0, 0, 0]);
    }
    out
}

fn kind<'a>() -> Finish<'a> {
    Finish {
        root_volume_uuid: "abcdef01-2345-6789-abcd-ef0123456789",
        boot_manifest_hash: false,
        launchd_catalog_sha256: None,
        a64x_cache_pretranslated: None,
        pretranslated_updates: None,
        trust_modules: TrustModules::default(),
    }
}

fn parsed(adt: &[u8]) -> Node {
    Node::parse(adt, Input::Base).unwrap()
}

fn find<'a>(tree: &'a Node, name: &str) -> &'a Node {
    tree.children.iter().find(|c| c.is_named(name.as_bytes())).unwrap()
}

fn names(node: &Node) -> Vec<&str> {
    node.properties
        .iter()
        .map(|p| std::str::from_utf8(&p.name).unwrap())
        .collect()
}

#[test]
fn a_written_tree_reads_back_the_same_and_drops_padding_and_bytes_after_a_name() {
    let mut raw = prop_word("odd", 0x8000_0003, b"abc");
    raw[4] = b'\0';
    raw[5] = b'z';
    raw[39] = 0xff;
    let data = node(&[raw], &[named("child", &[], &[])]);
    let tree = Node::parse(&data, Input::Base).unwrap();
    assert_eq!(tree.properties[0].name, b"odd");
    assert!(tree.properties[0].placeholder);
    let written = tree.serialize();
    assert_eq!(written[..4], data[..4]);
    assert_eq!(&written[8..40], &[b"odd".as_slice(), &[0u8; 29][..]].concat()[..]);
    assert_eq!(written[47], 0);
    assert_eq!(Node::parse(&written, Input::Base).unwrap(), tree);
}

#[test]
fn put_replaces_in_place_keeping_the_placeholder_bit_or_appends() {
    let mut tree = Node::parse(
        &node(&[prop_word("a", 0x8000_0001, b"1"), prop("b", b"2")], &[]),
        Input::Base,
    )
    .unwrap();
    tree.put("a", b"xyz".as_slice());
    tree.put("c", b"3".as_slice());
    assert_eq!(names(&tree), ["a", "b", "c"]);
    assert!(tree.properties[0].placeholder);
    assert_eq!(tree.first("a"), Some(&b"xyz"[..]));
}

#[test]
fn finish_gives_the_ipads_identity_in_adt_edits_order() {
    let adt = finish(&base(), &apple(), &kind()).unwrap();
    let tree = parsed(&adt);
    assert_eq!(
        names(&tree),
        [
            "name",
            "compatible",
            "model",
            "target-type",
            "platform-name",
            "target-sub-type"
        ]
    );
    assert_eq!(tree.first("compatible"), Some(&b"J181AP\0nintendo,nx\0"[..]));
    assert_eq!(tree.first("platform-name"), Some(&b"t8030\0"[..]));
    let chosen = find(&tree, "chosen");
    assert_eq!(
        names(chosen),
        [
            "name",
            "root-matching",
            "random-seed",
            "switch-whitepoint-type",
            "chip-id"
        ]
    );
    assert_eq!(chosen.first("chip-id"), Some(&0x8030u32.to_le_bytes()[..]));
    let matching = std::str::from_utf8(chosen.first("root-matching").unwrap()).unwrap();
    assert!(matching.contains("<string>ABCDEF01-2345-6789-ABCD-EF0123456789</string>"));
    let product = find(&tree, "product");
    assert_eq!(
        names(product),
        ["name", "product-name", "thin-bezel", "model", "artwork-display-gamut"]
    );
    assert_eq!(product.first("product-name"), Some(&b"iPad\0"[..]));
    let children: Vec<_> = product.children.iter().map(|c| c.first("name").unwrap()).collect();
    assert_eq!(children, [&b"maps\0"[..], b"audio\0"]);
    assert_eq!(
        names(find(product, "audio")),
        ["name", "wireless-splitter", "acoustic-id"]
    );
    assert_eq!(find(&tree, "arm-io").first("soc-generation"), Some(&b"H12\0"[..]));
}

#[test]
fn a_compatible_that_already_starts_with_the_board_is_kept() {
    let once = finish(&base(), &apple(), &kind()).unwrap();
    let twice = finish(&once, &apple(), &kind()).unwrap();
    assert_eq!(once, twice);
}

#[test]
fn finish_writes_each_kinds_chosen_entries_in_order() {
    let digests = [9u8; 64];
    let cache = [5u8; 40];
    let (system, translated, app) = (module(&[3, 9]), module(&[3, 5]), module(&[1]));
    let os_cryptex = [system.as_slice(), translated.as_slice()];
    let app_cryptex = [app.as_slice()];
    let adt = finish(
        &base(),
        &apple(),
        &Finish {
            boot_manifest_hash: true,
            launchd_catalog_sha256: Some(&digests),
            a64x_cache_pretranslated: Some(cache.as_slice()),
            trust_modules: TrustModules {
                os_cryptex: &os_cryptex,
                app_cryptex: &app_cryptex,
                pretranslated: &[],
            },
            ..kind()
        },
    )
    .unwrap();
    let tree = parsed(&adt);
    let chosen = find(&tree, "chosen");
    assert_eq!(
        names(chosen)[5..],
        [
            "boot-manifest-hash",
            "switch-launchd-catalog-sha256",
            "a64x-cache-pretranslated",
            "switch-os-cryptex-cdhashes",
            "switch-app-cryptex-cdhashes"
        ]
    );
    assert_eq!(chosen.first("boot-manifest-hash"), Some(&[0; 48][..]));
    assert_eq!(
        chosen.first("switch-os-cryptex-cdhashes"),
        Some(&[[3; 20], [5; 20], [9; 20]].concat()[..])
    );
    assert_eq!(chosen.first("switch-app-cryptex-cdhashes"), Some(&[1; 20][..]));
}

#[test]
fn finish_refuses_inputs_adt_edit_refuses() {
    let refused = |base: &[u8], apple: &[u8], kind: &Finish| finish(base, apple, kind).unwrap_err();
    let k = kind();
    assert!(matches!(
        refused(&base(), &[apple(), vec![0]].concat(), &k),
        Error::Trailing(Input::Apple, 1)
    ));
    assert!(matches!(
        refused(&base()[..40], &apple(), &k),
        Error::Truncated(Input::Base, _)
    ));
    assert!(
        matches!(refused(&base(), &apple_with(b"arm-io,t8030\0", 0), &k), Error::NoSingleNode(p) if p == "/backlight")
    );
    assert!(matches!(
        refused(&base(), &apple_with(b"arm-io,t8030\0", 2), &k),
        Error::NoSingleNode(_)
    ));
    assert!(matches!(
        refused(&base(), &apple_with(b"soc,t8030\0", 1), &k),
        Error::NoSoc
    ));
    assert!(matches!(
        refused(&base(), &apple_with(b"arm-io,tzz\0", 1), &k),
        Error::NoSoc
    ));
    let unicode = Finish {
        root_volume_uuid: "é",
        ..k
    };
    assert!(matches!(refused(&base(), &apple(), &unicode), Error::VolumeUuid(_)));
    for len in [0, 21] {
        let list = vec![0u8; len];
        let given = Finish {
            a64x_cache_pretranslated: Some(list.as_slice()),
            ..k
        };
        assert!(matches!(refused(&base(), &apple(), &given), Error::PretranslatedList(l) if l == len));
    }
}

#[test]
fn finish_refuses_a_bad_trust_module_or_an_empty_role() {
    let k = kind();
    let unsorted = module(&[2, 1]);
    let mut version = module(&[1]);
    version[0] = 1;
    let short = module(&[1])[..30].to_vec();
    for bad in [unsorted, version, short] {
        let modules = [bad.as_slice()];
        let given = Finish {
            trust_modules: TrustModules {
                app_cryptex: &modules,
                ..k.trust_modules
            },
            ..k
        };
        assert!(matches!(finish(&base(), &apple(), &given), Err(Error::TrustModule(_))));
    }
    let empty = module(&[]);
    let modules = [empty.as_slice()];
    let given = Finish {
        trust_modules: TrustModules {
            pretranslated: &modules,
            ..k.trust_modules
        },
        ..k
    };
    assert!(matches!(
        finish(&base(), &apple(), &given),
        Err(Error::NoCdhashes("pretranslated"))
    ));
}

#[test]
fn finish_refuses_what_the_layout_step_cannot_read() {
    let k = kind();
    let no_sep = named(
        "device-tree",
        &[prop("compatible", b"x\0")],
        &[named("chosen", &[], &[])],
    );
    assert!(matches!(
        finish(&no_sep, &apple(), &k),
        Err(Error::Invalid("no /filesystems DoNotUseSEP"))
    ));
    let twins = named(
        "device-tree",
        &[prop("compatible", b"x\0")],
        &[named("chosen", &[], &[]), named("x", &[], &[]), named("x", &[], &[])],
    );
    assert!(matches!(
        finish(&twins, &apple(), &k),
        Err(Error::Invalid("two sibling nodes share a name"))
    ));
    let no_compatible = named("device-tree", &[], &[]);
    assert!(matches!(finish(&no_compatible, &apple(), &k), Err(Error::NoCompatible)));
}

#[test]
fn finish_writes_and_validates_pretranslated_updates() {
    let pt = module(&[3, 5, 9]);
    let pt_modules = [pt.as_slice()];
    let updates = format!(
        r#"{{"/usr/libexec/locationd":"{}","/usr/bin/a":"{}"}}"#,
        "09".repeat(20),
        "03".repeat(20),
    );
    let adt = finish(
        &base(),
        &apple(),
        &Finish {
            pretranslated_updates: Some(updates.as_bytes()),
            trust_modules: TrustModules {
                pretranslated: &pt_modules,
                ..TrustModules::default()
            },
            ..kind()
        },
    )
    .unwrap();
    let tree = parsed(&adt);
    let raw = find(&tree, "chosen")
        .first("switch-pretranslated-updates")
        .expect("switch-pretranslated-updates");
    let strings = b"/usr/bin/a\0/usr/libexec/locationd\0";
    let mut expected = Vec::new();
    expected.extend_from_slice(b"SWUP");
    expected.extend_from_slice(&1u32.to_le_bytes());
    expected.extend_from_slice(&2u32.to_le_bytes());
    expected.extend_from_slice(&(strings.len() as u32).to_le_bytes());
    expected.extend_from_slice(&0u32.to_le_bytes());
    expected.extend_from_slice(&[3u8; 20]);
    expected.extend_from_slice(&11u32.to_le_bytes());
    expected.extend_from_slice(&[9u8; 20]);
    expected.extend_from_slice(strings);
    assert_eq!(raw, expected.as_slice());

    let untrusted = format!(r#"{{"/usr/libexec/locationd":"{}"}}"#, "04".repeat(20));
    let err = finish(
        &base(),
        &apple(),
        &Finish {
            pretranslated_updates: Some(untrusted.as_bytes()),
            trust_modules: TrustModules {
                pretranslated: &pt_modules,
                ..TrustModules::default()
            },
            ..kind()
        },
    )
    .unwrap_err();
    assert!(matches!(err, Error::PretranslatedUpdates(ref m) if m.contains("/usr/libexec/locationd")));

    let bad_path = format!(r#"{{"relative/path":"{}"}}"#, "03".repeat(20));
    let err = finish(
        &base(),
        &apple(),
        &Finish {
            pretranslated_updates: Some(bad_path.as_bytes()),
            trust_modules: TrustModules {
                pretranslated: &pt_modules,
                ..TrustModules::default()
            },
            ..kind()
        },
    )
    .unwrap_err();
    assert!(matches!(err, Error::PretranslatedUpdates(_)));
}

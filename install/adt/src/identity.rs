// SPDX-License-Identifier: GPL-3.0-or-later
//! adt_edit.py with_ipad_identity: the identity of the iPad whose device tree Apple ships in the IPSW. Every put is in
//! adt_edit.py's order, since a new property goes after the node's others.

use crate::Error;
use crate::tree::{Node, until_nul};

/// adt_edit.py PRODUCT_IDENTITY.
const PRODUCT_IDENTITY: &[&str] = &[
    "product-name",
    "product-description",
    "artwork-device-idiom",
    "artwork-device-subtype",
    "artwork-scale-factor",
    "compatible-device-fallback",
    "thin-bezel",
    "product-id",
    "graphics-featureset-class",
    "graphics-featureset-fallbacks",
    "device-perf-memory-class",
    "app-macho-architecture",
    "framebuffer-identifier",
    "display-mirroring",
    "ui-background-quality",
    "ui-weather-quality",
    "bluetooth-le",
    "ui-pip",
    "ui-pinned-app",
    "ui-floating-live-app",
    "ui-overlay-app",
    "hearingaid-audio-equalization",
    "hearingaid-low-energy-audio",
    "iap2-protocol-supported",
    "allow-32bit-apps",
    "partition-style",
    "ephemeral-data-mode",
    "multiuser-sessions",
    "has-applelpm",
    "has-boot-chime",
    "udid-version",
    "chrome-identifier",
    "device-color-policy",
    "itunes-min-ver",
    "mobiledevice-min-ver",
    "ptp-large-files",
    "RF-exposure-separation-distance",
    "disable-globe-map",
    "sandman-support",
    "supports-avatars",
    "wifi-chipset",
    "sub-product-type",
    "fdr-product-type",
    "unique-model",
    "has-exclaves",
    "exclaves-enabled",
];

/// adt_edit.py AUDIO_IDENTITY.
const AUDIO_IDENTITY: [&str; 1] = ["acoustic-id"];
/// adt_edit.py DISPLAY_GAMUT.
const DISPLAY_GAMUT: &[u8] = b"sRGB\0";
/// adt_edit.py WHITEPOINT_TYPE.
const WHITEPOINT_TYPE: &str = "switch-whitepoint-type";

fn apple_node<'a>(apple: &'a Node, path: &str) -> Result<&'a Node, Error> {
    let mut node = apple;
    for name in path.split('/').filter(|name| !name.is_empty()) {
        let mut found = node.children.iter().filter(|c| c.is_named(name.as_bytes()));
        node = match (found.next(), found.next()) {
            (Some(only), None) => only,
            _ => return Err(Error::NoSingleNode(path.to_owned())),
        };
    }
    Ok(node)
}

fn apple_value<'a>(apple: &'a Node, path: &str, name: &str) -> Result<&'a [u8], Error> {
    apple_node(apple, path)?.last(name).ok_or_else(|| Error::NoProperty {
        path: path.to_owned(),
        name: name.to_owned(),
    })
}

/// The SoC as iBoot names it in platform-name (`t8011\0`), from Apple's /arm-io compatible `arm-io,t8011`.
fn platform_name(apple: &Node) -> Result<Vec<u8>, Error> {
    let compatible = until_nul(apple_value(apple, "/arm-io", "compatible")?);
    let (family, soc) = match compatible.iter().position(|&b| b == b',') {
        Some(comma) => (&compatible[..comma], &compatible[comma + 1..]),
        None => (compatible, &b""[..]),
    };
    if family != b"arm-io" || !soc.starts_with(b"t") {
        return Err(Error::NoSoc);
    }
    Ok([soc, b"\0"].concat())
}

fn chip_id(platform_name: &[u8]) -> Result<u32, Error> {
    let digits = &platform_name[1..platform_name.len() - 1];
    std::str::from_utf8(digits)
        .ok()
        .and_then(|digits| u32::from_str_radix(digits, 16).ok())
        .ok_or(Error::NoSoc)
}

fn product(tree: &mut Node, apple: &Node) -> Result<(), Error> {
    let identity = apple_node(apple, "/product")?;
    let model = apple_value(apple, "/", "model")?;
    let product = tree.child("product");
    for key in PRODUCT_IDENTITY {
        if let Some(value) = identity.last(key) {
            product.put(key, value);
        }
    }
    product.put("model", model);
    product.put("artwork-display-gamut", DISPLAY_GAMUT);
    let audio = product.child("audio");
    audio.put("wireless-splitter", 1u32.to_le_bytes());
    let apple_audio = apple_node(apple, "/product/audio")?;
    for key in AUDIO_IDENTITY {
        if let Some(value) = apple_audio.last(key) {
            audio.put(key, value);
        }
    }
    Ok(())
}

pub(crate) fn give(tree: &mut Node, apple: &Node) -> Result<(), Error> {
    tree.put("model", apple_value(apple, "/", "model")?);
    tree.put("target-type", apple_value(apple, "/", "target-type")?);
    let board = [until_nul(apple_value(apple, "/", "compatible")?), b"\0"].concat();
    let compatible = tree.first("compatible").ok_or(Error::NoCompatible)?.to_vec();
    if !compatible.starts_with(&board) {
        tree.put("compatible", [board, compatible].concat());
    }
    product(tree, apple)?;
    if let Some(whitepoint) = apple_node(apple, "/backlight")?.last("default-whitepoint-type") {
        tree.child("chosen").put(WHITEPOINT_TYPE, whitepoint);
    }
    let soc = platform_name(apple)?;
    let chip_id = chip_id(&soc)?;
    tree.put("platform-name", soc);
    tree.put("target-sub-type", apple_value(apple, "/", "target-sub-type")?);
    tree.child("chosen").put("chip-id", chip_id.to_le_bytes());
    tree.child("arm-io")
        .put("soc-generation", apple_value(apple, "/arm-io", "soc-generation")?);
    Ok(())
}

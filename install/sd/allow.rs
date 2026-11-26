// SPDX-License-Identifier: GPL-3.0-or-later
//! The only paths the installer writes on the card's FAT: what hekate's `iOS` entry starts from.

use crate::Error;

pub const BOOT_DIR: &str = "switchroot/ios";
pub const INI: &str = "bootloader/ini/ios.ini";
pub const ICON: &str = "bootloader/res/icon_ios_hue.bmp";
/// The user's boot arguments, which the loader reads beside the boot files and no package ships.
pub const BOOT_ARGS: &str = "bootargs.txt";

/// `path` is `switchroot/ios/<name>` (never the user's bootargs.txt), `ios.ini` or the icon. FAT names compare
/// without case, so the checks do too.
pub fn check(path: &str) -> Result<(), Error> {
    let refuse = |why| {
        Err(Error::Refused {
            path: path.to_string(),
            why,
        })
    };
    if path.contains('\\') || path.split('/').any(|c| c.is_empty() || c == "." || c == "..") {
        return refuse("not a plain relative path");
    }
    if path.eq_ignore_ascii_case(INI) || path.eq_ignore_ascii_case(ICON) {
        return Ok(());
    }
    match path.rsplit_once('/') {
        Some((dir, name)) if dir.eq_ignore_ascii_case(BOOT_DIR) => {
            if user_owned(name) {
                refuse("the user's boot arguments")
            } else {
                Ok(())
            }
        }
        _ => refuse("outside switchroot/ios/, bootloader/ini/ios.ini and bootloader/res/icon_ios_hue.bmp"),
    }
}

/// Files in switchroot/ios/ that no package owns, so a replace never removes them: the user's boot arguments (and
/// macOS's AppleDouble beside them), and the retired installer's image parts (`IOS17.SII.001`, ...).
pub fn user_owned(name: &str) -> bool {
    let name = name.strip_prefix("._").unwrap_or(name);
    name.eq_ignore_ascii_case(BOOT_ARGS) || image_part(name)
}

fn image_part(name: &str) -> bool {
    let Some((stem, part)) = name.rsplit_once('.') else {
        return false;
    };
    let Some(stem) = stem.strip_suffix(".SII") else {
        return false;
    };
    part.len() == 3
        && part.bytes().all(|b| b.is_ascii_digit())
        && !stem.is_empty()
        && stem.bytes().all(|b| b.is_ascii_uppercase() || b.is_ascii_digit())
}

/// Every directory the allowlisted paths pass through, parents first.
pub fn directories() -> Vec<&'static str> {
    let mut dirs: Vec<&'static str> = Vec::new();
    for path in [INI, ICON, BOOT_DIR] {
        let ends = path.match_indices('/').map(|(i, _)| i);
        let ends = if path == BOOT_DIR {
            ends.chain([path.len()]).collect()
        } else {
            ends.collect::<Vec<_>>()
        };
        for end in ends {
            if !dirs.contains(&&path[..end]) {
                dirs.push(&path[..end]);
            }
        }
    }
    dirs
}

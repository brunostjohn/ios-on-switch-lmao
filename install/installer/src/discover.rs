// SPDX-License-Identifier: GPL-3.0-or-later
use std::path::Path;

use switch_disk::{Disk, OpenOptions};
use switch_engine::{CardProbe, TargetSpec, probe_image};
use switch_tui::CardOption;

use crate::convert::{disk_confirm_token, disk_to_card_option, image_to_card_option};

#[derive(Clone, Debug)]
pub struct DiscoveredCard {
    pub option: CardOption,
    pub target: TargetSpec,
    pub probe: Option<CardProbe>,
}

pub fn discover_cards(eject: bool, force_unmount: bool) -> Result<Vec<DiscoveredCard>, String> {
    let disks = switch_disk::list().map_err(|e| e.to_string())?;
    let mut out = Vec::new();
    for disk in disks {
        if !disk.pickable() {
            continue;
        }
        let option = disk_to_card_option(&disk, None);
        let target = TargetSpec::Disk {
            disk,
            options: OpenOptions {
                force_unmount,
                ..OpenOptions::default()
            },
            eject,
        };
        out.push(DiscoveredCard {
            option,
            target,
            probe: None,
        });
    }
    Ok(out)
}

pub fn discover_image_card(path: &Path, image_used: u64) -> Result<DiscoveredCard, String> {
    let probe = probe_image(path, image_used).map_err(|e| e.to_string())?;
    let option = image_to_card_option(path, &probe);
    Ok(DiscoveredCard {
        option,
        target: TargetSpec::image_file(path),
        probe: Some(probe),
    })
}

pub fn match_card_selector<'a>(cards: &'a [DiscoveredCard], selector: &str) -> Option<&'a DiscoveredCard> {
    cards.iter().find(|c| {
        if c.option.id == selector || c.option.effective_confirm_token().eq_ignore_ascii_case(selector) {
            return true;
        }
        match &c.target {
            TargetSpec::Disk { disk, .. } => disk_matches(disk, selector),
            TargetSpec::ImageFile { path, .. } => path.to_string_lossy() == selector,
        }
    })
}

fn disk_matches(disk: &Disk, selector: &str) -> bool {
    disk.id == selector
        || disk.path.to_string_lossy() == selector
        || disk_confirm_token(disk).eq_ignore_ascii_case(selector)
}

// SPDX-License-Identifier: GPL-3.0-or-later
use std::env;
use std::fs;
use std::path::Path;
use std::process::ExitCode;

use switch_image_overlay::{TreeSource, tree_digest, verify_embedded_profile};

fn main() -> ExitCode {
    let args: Vec<String> = env::args().collect();
    let result = match args.get(1).map(String::as_str) {
        Some("verify-profile") => match args.get(2) {
            Some(path) => fs::read(path)
                .map_err(switch_image_overlay::Error::from)
                .and_then(|bytes| verify_embedded_profile(path, &bytes)),
            None => Err(switch_image_overlay::Error::Source(
                "usage: switch_image_overlay_tool verify-profile <path>".into(),
            )),
        },
        Some("tree-digest") => match args.get(2) {
            Some(path) => TreeSource::from_dir(Path::new(path)).map(|src| {
                println!("{}", tree_digest(&src));
            }),
            None => Err(switch_image_overlay::Error::Source(
                "usage: switch_image_overlay_tool tree-digest <dir>".into(),
            )),
        },
        _ => Err(switch_image_overlay::Error::Source(
            "usage: switch_image_overlay_tool <verify-profile|tree-digest> <path>".into(),
        )),
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(err) => {
            eprintln!("{err}");
            ExitCode::FAILURE
        }
    }
}

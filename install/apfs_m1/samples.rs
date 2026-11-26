// SPDX-License-Identifier: GPL-3.0-or-later
//! `build --sample`: content made through the writer's API, as W5–W8 will make it, beside the copy. `replace` names
//! files of the source `api_fixture_test.sh` makes.

use std::io;
use std::sync::Arc;

use switch_apfs_write::{Attrs, Content, Sequential, Tree, Xattr, XattrValue};

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

#[derive(clap::ValueEnum, Clone, Copy, Debug)]
pub enum Sample {
    /// Whole-file clones, one pair on each side of an inode number's low byte; a hard link with a streamed xattr; a
    /// symlink.
    Clones,
    /// Two files whose xattrs name one stream.
    SharedXattr,
    /// One name in NFC and again in NFD, in one directory.
    NfdTwin,
    /// New content for copied files: a plain file, a compressed clone and a whole-file clone.
    Replace,
    /// A Preboot file whose fill writes 10 bytes of its three blocks.
    ShortFill,
}

fn file() -> Attrs {
    Attrs::new(0, 0, 0o644)
}

pub fn add(sample: Sample, data: &mut Tree, preboot: &mut Tree) -> Result<()> {
    match sample {
        Sample::Clones => clones(data),
        Sample::SharedXattr => {
            let dir = data.add_dir(data.root(), "shared-xattr", Attrs::new(0, 0, 0o755))?;
            let shared = data.add_bytes(vec![5u8; 9000]);
            for name in ["s1", "s2"] {
                let f = data.add_file(dir, name, file(), None)?;
                data.put_xattr(
                    f,
                    Xattr {
                        name: "com.example.big".into(),
                        flags: 0x10,
                        value: XattrValue::Stream(shared),
                    },
                );
            }
            Ok(())
        }
        Sample::NfdTwin => {
            let dir = data.add_dir(data.root(), "nfd-twin", Attrs::new(0, 0, 0o755))?;
            data.add_file(dir, "caf\u{e9}", file(), None)?;
            data.add_file(dir, "cafe\u{301}", file(), None)?;
            Ok(())
        }
        Sample::Replace => {
            for (path, repeat) in [("d/plain", 1), ("c/words", 2000), ("d/orig", 3)] {
                let inode = data.lookup(path).ok_or_else(|| format!("{path}: not in the copy"))?;
                let bytes = format!("replaced {path}\n").repeat(repeat).into_bytes();
                let stream = data.add_bytes(bytes);
                data.replace_content(inode, Some(stream))?;
            }
            Ok(())
        }
        Sample::ShortFill => {
            let short = Sequential::new(10, |out: &mut dyn FnMut(&[u8]) -> io::Result<()>| out(&[1u8; 10]));
            let stream = preboot.add_stream(3 * 4096, Content::Fill(Arc::new(short)));
            preboot.add_file(preboot.root(), "short.img", file(), Some(stream))?;
            Ok(())
        }
    }
}

fn clones(t: &mut Tree) -> Result<()> {
    let dir = t.add_dir(t.root(), "api", Attrs::new(0, 0, 0o755))?;
    let pattern: Vec<u8> = (0..65536u32).map(|i| (i % 251) as u8).collect();
    let s = t.add_bytes(pattern);
    t.add_file(dir, "x", file(), Some(s))?;
    t.add_file(dir, "y", file(), Some(s))?;
    let mut pad = 0;
    for (low, name, byte) in [(0xff, "a", 1u8), (0x01, "b", 2u8)] {
        while t.next_number() % 256 != low {
            t.add_file(dir, &format!("pad{pad}"), file(), None)?;
            pad += 1;
        }
        let s = t.add_bytes(vec![byte; 4096]);
        t.add_file(dir, &format!("{name}1"), file(), Some(s))?;
        t.add_file(dir, &format!("{name}2"), file(), Some(s))?;
    }
    let h = t.add_bytes(&b"hard\n"[..]);
    let hard = t.add_file(dir, "h1", file(), Some(h))?;
    t.add_link(hard, dir, "h2")?;
    t.set_xattr(hard, "com.example.big", &[3u8; 9000]);
    t.add_symlink(dir, "ln", Attrs::new(0, 0, 0o755), "x")?;
    Ok(())
}

// SPDX-License-Identifier: GPL-3.0-or-later
use std::fs;
use std::path::PathBuf;
use std::sync::Arc;

use sha2::{Digest, Sha256};
use switch_apfs_write::{Attrs, Content, Fill, Tree};

use crate::fill::{BytesFill, FileFill};
use crate::{Error, STAGING_DIR};

const PREBOOT_MOUNT_PREFIX: &str = "/private/preboot/Switch-Staging/";

pub enum CryptexContent {
    File {
        path: PathBuf,
        expected_sha256: Option<[u8; 32]>,
    },
    Bytes {
        bytes: Arc<[u8]>,
        expected_sha256: Option<[u8; 32]>,
    },
    Fill {
        size: u64,
        fill: Arc<dyn Fill>,
    },
}

pub struct CryptexImage {
    pub name: String,
    pub content: CryptexContent,
}

pub fn staging_name(preboot_path: &str) -> Result<&str, Error> {
    let name = preboot_path
        .strip_prefix(PREBOOT_MOUNT_PREFIX)
        .ok_or_else(|| Error::InvalidCryptex(format!("{preboot_path:?} is not under {PREBOOT_MOUNT_PREFIX}")))?;
    check_name(name)?;
    Ok(name)
}

fn check_name(name: &str) -> Result<(), Error> {
    if name.is_empty() || name == "." || name == ".." || name.contains(['/', '\0']) {
        return Err(Error::InvalidCryptex(format!("{name:?} is not a staging file name")));
    }
    Ok(())
}

/// Builds Preboot: root `0:0 0755`, `Switch-Staging` `0:0 0755`, and each cryptex `0:0 0644` placed via
/// `Content::Fill` so APFS allocates a single contiguous extent (`SwitchCryptex.cpp:130-158`).
pub fn build_preboot(images: &[CryptexImage]) -> Result<Tree, Error> {
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    let staging = tree.add_dir(tree.root(), STAGING_DIR, Attrs::new(0, 0, 0o755))?;
    for image in images {
        check_name(&image.name)?;
        let (size, fill): (u64, Arc<dyn Fill>) = match &image.content {
            CryptexContent::File { path, expected_sha256 } => {
                let meta = fs::metadata(path)?;
                if !meta.is_file() {
                    return Err(Error::NotRegularFile(path.display().to_string()));
                }
                let size = meta.len();
                if size == 0 {
                    return Err(Error::InvalidCryptex(format!("{}: empty image", image.name)));
                }
                (size, Arc::new(FileFill::new(path.clone(), size, *expected_sha256)))
            }
            CryptexContent::Bytes { bytes, expected_sha256 } => {
                if bytes.is_empty() {
                    return Err(Error::InvalidCryptex(format!("{}: empty image", image.name)));
                }
                if let Some(want) = expected_sha256 {
                    let got: [u8; 32] = Sha256::digest(bytes).into();
                    if got != *want {
                        return Err(Error::DigestMismatch {
                            name: image.name.clone(),
                            got: hex::encode(got),
                            want: hex::encode(want),
                        });
                    }
                }
                (bytes.len() as u64, Arc::new(BytesFill::new(bytes.clone())))
            }
            CryptexContent::Fill { size, fill } => {
                if *size == 0 {
                    return Err(Error::InvalidCryptex(format!("{}: empty image", image.name)));
                }
                (*size, fill.clone())
            }
        };
        let stream = tree.add_stream(size, Content::Fill(fill));
        tree.add_file(staging, &image.name, Attrs::new(0, 0, 0o644), Some(stream))?;
    }
    Ok(tree)
}

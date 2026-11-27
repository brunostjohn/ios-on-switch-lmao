// SPDX-License-Identifier: GPL-3.0-or-later
use switch_apfs_read::Fs;
use switch_apfs_write::{Attrs, Copied, Tree, copy_subtree};

use crate::path::{ensure_dir, ensure_dir_under};
use crate::{Error, MOBILE_GID, MOBILE_UID, VAR_PREFIX};

pub struct SeededData {
    pub tree: Tree,
    pub copied: Copied,
}

/// Seeds Data from System's `/private/var` and ensures `mobile/Library` and `mobile/Library/Preferences` exist with
/// mobile's owner (`tools/install/populate.py:53-59`).
pub fn seed_data(system: &Fs) -> Result<SeededData, Error> {
    let mut tree = Tree::new(Attrs::new(0, 0, 0o755));
    let root = tree.root();
    let copied = copy_subtree(system, VAR_PREFIX, &mut tree, root)?;

    let mobile_dir = Attrs::new(MOBILE_UID, MOBILE_GID, 0o755);
    let mobile = ensure_dir(&mut tree, "mobile", mobile_dir.clone())?;
    let library = ensure_dir_under(&mut tree, mobile, "Library", mobile_dir.clone())?;
    let prefs = ensure_dir_under(&mut tree, library, "Preferences", mobile_dir)?;
    for dir in [library, prefs] {
        let attrs = &mut tree.inode_mut(dir).attrs;
        attrs.uid = MOBILE_UID;
        attrs.gid = MOBILE_GID;
    }

    Ok(SeededData { tree, copied })
}

// SPDX-License-Identifier: GPL-3.0-or-later
use object::LittleEndian as LE;
use object::macho::{self, FilesetEntryCommand, LinkeditDataCommand};

use super::{Command, Image, read};
use crate::error::{Result, refused};

pub(crate) struct FilesetEntry {
    pub id: String,
    pub va: u64,
    pub fileoff: u64,
}

/// A kernel collection's `LC_FILESET_ENTRY`s in load command order, as `tools/common/macho.fileset_entries` reads
/// them: the id runs from its offset in the command to the first NUL.
pub(crate) fn fileset_entries(data: &[u8], image: &Image) -> Result<Vec<FilesetEntry>> {
    let mut entries = Vec::new();
    for command in image.commands.iter().filter(|c| c.cmd == macho::LC_FILESET_ENTRY.0) {
        if command.size < size_of::<FilesetEntryCommand<LE>>() {
            return refused("Truncated fileset entry");
        }
        let entry: &FilesetEntryCommand<LE> = read(data, command.offset)?;
        let id_at = entry.entry_id.offset.get(LE) as usize;
        let Some(raw) = data.get(command.offset + id_at.min(command.size)..command.offset + command.size) else {
            return refused("Fileset entry outside image");
        };
        let raw = raw.split(|&b| b == 0).next().unwrap_or_default();
        let Ok(id) = std::str::from_utf8(raw) else {
            return refused("A fileset entry id that is not UTF-8");
        };
        entries.push(FilesetEntry {
            id: id.to_owned(),
            va: entry.vmaddr.get(LE),
            fileoff: entry.fileoff.get(LE),
        });
    }
    Ok(entries)
}

pub(crate) fn linkedit_data(data: &[u8], command: &Command) -> Result<(usize, usize)> {
    if command.size < size_of::<LinkeditDataCommand<LE>>() {
        return refused("Truncated linkedit data command");
    }
    let linkedit: &LinkeditDataCommand<LE> = read(data, command.offset)?;
    Ok((linkedit.dataoff.get(LE) as usize, linkedit.datasize.get(LE) as usize))
}

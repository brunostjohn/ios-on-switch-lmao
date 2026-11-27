// SPDX-License-Identifier: GPL-3.0-or-later
use std::collections::HashMap;
use std::collections::hash_map::Entry;

use object::LittleEndian as LE;
use object::ReadRef;
use object::macho::{self, N_EXT, N_STAB, N_TYPE, Nlist64, SymtabCommand};

use super::{Image, read};
use crate::error::{Result, refused};

/// The names of the image's defined external symbols by address, in symbol table order.
pub(crate) fn exports(data: &[u8], image: &Image) -> Result<HashMap<u64, Vec<String>>> {
    let mut result: HashMap<u64, Vec<String>> = HashMap::new();
    for (address, name) in defined_externals(data, image)? {
        result.entry(address).or_default().push(name.to_owned());
    }
    Ok(result)
}

/// Each defined external name's address as `emit26.by_name` picks it from `exports`' dict: of the addresses that
/// carry the name, the one the symbol table reaches first.
pub(crate) fn export_addresses(data: &[u8], image: &Image) -> Result<HashMap<String, u64>> {
    let mut first_seen: HashMap<u64, usize> = HashMap::new();
    let mut chosen: HashMap<String, (usize, u64)> = HashMap::new();
    for (index, (address, name)) in defined_externals(data, image)?.into_iter().enumerate() {
        let rank = *first_seen.entry(address).or_insert(index);
        match chosen.entry(name.to_owned()) {
            Entry::Vacant(slot) => {
                slot.insert((rank, address));
            }
            Entry::Occupied(mut slot) if rank < slot.get().0 => {
                slot.insert((rank, address));
            }
            Entry::Occupied(_) => {}
        }
    }
    Ok(chosen.into_iter().map(|(name, (_, address))| (name, address)).collect())
}

/// `(address, name)` of each external definition, in symbol table order.
///
/// Only the names of external definitions are read: a kernel holds ten times as many local symbols as exports.
fn defined_externals<'a>(data: &'a [u8], image: &Image) -> Result<Vec<(u64, &'a str)>> {
    let mut tables = image.commands.iter().filter(|c| c.cmd == macho::LC_SYMTAB.0);
    let (Some(table), None) = (tables.next(), tables.next()) else {
        return refused("Expected one symbol table");
    };
    if table.size != size_of::<SymtabCommand<LE>>() {
        return refused("Expected one symbol table");
    }
    let symtab: &SymtabCommand<LE> = read(data, table.offset)?;
    let (symoff, count) = (symtab.symoff.get(LE) as usize, symtab.nsyms.get(LE) as usize);
    let (stroff, strsize) = (symtab.stroff.get(LE) as usize, symtab.strsize.get(LE) as usize);
    if symoff + count * size_of::<Nlist64<LE>>() > data.len() || stroff + strsize > data.len() {
        return refused("Symbol table outside image");
    }
    let strings = &data[stroff..stroff + strsize];
    let Ok(nlists) = data.read_slice_at::<Nlist64<LE>>(symoff as u64, count) else {
        return refused("Symbol table outside image");
    };
    let mut found = Vec::new();
    for symbol in nlists.iter().filter(|s| defined_external(s.n_type.0)) {
        let name = strings
            .get(symbol.n_strx.get(LE) as usize..)
            .and_then(|rest| rest.iter().position(|&b| b == 0).map(|end| &rest[..end]));
        let Some(Ok(name)) = name.map(std::str::from_utf8) else {
            return refused("Unterminated symbol name");
        };
        found.push((symbol.n_value.get(LE), name));
    }
    Ok(found)
}

/// An external definition: in a section, absolute or indirect, so any type but undefined.
fn defined_external(kind: u8) -> bool {
    kind & N_STAB == 0 && kind & N_EXT.0 != 0 && kind & N_TYPE != 0
}

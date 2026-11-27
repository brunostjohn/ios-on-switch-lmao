// SPDX-License-Identifier: GPL-3.0-or-later
//! `plistlib._BinaryPlistWriter` with `sort_keys=False`.

use std::collections::HashMap;

use super::{Error, Id, Object, Plist, date};

/// `_flatten`'s `(type(value), value)` key. `None` is one object; NaN, UIDs and containers have none and go by
/// identity.
#[derive(Hash, PartialEq, Eq)]
enum Scalar<'a> {
    Null,
    Bool(bool),
    Int(i128),
    Real(u64),
    Date(i64),
    Data(&'a [u8]),
    Str(&'a str),
}

fn scalar(object: &Object) -> Option<Scalar<'_>> {
    Some(match object {
        Object::Null => Scalar::Null,
        Object::Bool(value) => Scalar::Bool(*value),
        Object::Int(value) => Scalar::Int(*value),
        Object::Real(value) if value.is_nan() => return None,
        Object::Real(value) => Scalar::Real(if *value == 0.0 { 0 } else { value.to_bits() }),
        Object::Date(value) => Scalar::Date(*value),
        Object::Data(value) => Scalar::Data(value),
        Object::String(value) => Scalar::Str(value),
        Object::BigInt | Object::Uid(_) | Object::Array(_) | Object::Dict(_) => return None,
    })
}

enum Item<'a> {
    Object(Id),
    Key(&'a str),
}

struct Table<'a> {
    order: Vec<Item<'a>>,
    by_id: Vec<Option<usize>>,
    by_value: HashMap<Scalar<'a>, usize>,
}

impl<'a> Table<'a> {
    /// `_flatten` from the root: each object before its children, a dictionary's keys before its values, and an object
    /// seen before (by value or identity) not again. Marking an object when it leaves the stack visits the same order
    /// as plistlib's recursion.
    fn flatten(plist: &'a Plist) -> Table<'a> {
        let mut table = Table {
            order: Vec::new(),
            by_id: vec![None; plist.objects.len()],
            by_value: HashMap::new(),
        };
        let mut stack = vec![Item::Object(plist.root)];
        while let Some(item) = stack.pop() {
            let id = match item {
                Item::Key(key) => {
                    if !table.by_value.contains_key(&Scalar::Str(key)) {
                        table.by_value.insert(Scalar::Str(key), table.order.len());
                        table.order.push(item);
                    }
                    continue;
                }
                Item::Object(id) => id,
            };
            if table.by_id[id].is_some() {
                continue;
            }
            let object = &plist.objects[id];
            if let Some(key) = scalar(object) {
                if let Some(&number) = table.by_value.get(&key) {
                    table.by_id[id] = Some(number);
                    continue;
                }
                table.by_value.insert(key, table.order.len());
            }
            table.by_id[id] = Some(table.order.len());
            table.order.push(item);
            match object {
                Object::Array(items) => stack.extend(items.iter().rev().map(|id| Item::Object(*id))),
                Object::Dict(entries) => {
                    stack.extend(entries.iter().rev().map(|(_, value)| Item::Object(*value)));
                    stack.extend(entries.iter().rev().map(|(key, _)| Item::Key(key)));
                }
                _ => {}
            }
        }
        table
    }

    fn number(&self, id: Id) -> usize {
        self.by_id[id].expect("every reachable object is flattened")
    }

    fn key_number(&self, key: &str) -> usize {
        self.by_value[&Scalar::Str(key)]
    }
}

fn count_to_size(count: u64) -> usize {
    match count {
        0..0x100 => 1,
        0x100..0x1_0000 => 2,
        0x1_0000..0x1_0000_0000 => 4,
        _ => 8,
    }
}

fn push_sized(out: &mut Vec<u8>, value: u64, size: usize) {
    out.extend_from_slice(&value.to_be_bytes()[8 - size..]);
}

fn push_size(out: &mut Vec<u8>, token: u8, size: usize) {
    let size = size as u64;
    if size < 15 {
        out.push(token | size as u8);
        return;
    }
    let width = count_to_size(size);
    out.extend_from_slice(&[token | 0xf, 0x10 | width.trailing_zeros() as u8]);
    push_sized(out, size, width);
}

fn push_int(out: &mut Vec<u8>, value: i128) -> Result<(), Error> {
    if value < 0 {
        let value = i64::try_from(value).map_err(|_| Error::IntOverflow)?;
        out.push(0x13);
        out.extend_from_slice(&value.to_be_bytes());
    } else if value < 1 << 63 {
        let width = count_to_size(value as u64);
        out.push(0x10 | width.trailing_zeros() as u8);
        push_sized(out, value as u64, width);
    } else if value < 1 << 64 {
        out.push(0x14);
        out.extend_from_slice(&value.to_be_bytes());
    } else {
        return Err(Error::IntOverflow);
    }
    Ok(())
}

fn push_uid(out: &mut Vec<u8>, value: u64) {
    let width = count_to_size(value);
    out.push(0x80 | (width - 1) as u8);
    push_sized(out, value, width);
}

fn push_string(out: &mut Vec<u8>, value: &str) {
    if value.is_ascii() {
        push_size(out, 0x50, value.len());
        out.extend_from_slice(value.as_bytes());
    } else {
        let units: Vec<u16> = value.encode_utf16().collect();
        push_size(out, 0x60, units.len());
        out.extend(units.iter().flat_map(|unit| unit.to_be_bytes()));
    }
}

pub fn write(plist: &Plist) -> Result<Vec<u8>, Error> {
    let table = Table::flatten(plist);
    let count = table.order.len();
    let ref_size = count_to_size(count as u64);
    let mut out = b"bplist00".to_vec();
    let mut offsets = Vec::with_capacity(count);
    for item in &table.order {
        offsets.push(out.len() as u64);
        let id = match item {
            Item::Key(key) => {
                push_string(&mut out, key);
                continue;
            }
            Item::Object(id) => *id,
        };
        match &plist.objects[id] {
            Object::Null => out.push(0x00),
            Object::Bool(false) => out.push(0x08),
            Object::Bool(true) => out.push(0x09),
            Object::Int(value) => push_int(&mut out, *value)?,
            Object::BigInt => return Err(Error::IntOverflow),
            Object::Real(value) => {
                out.push(0x23);
                out.extend_from_slice(&value.to_be_bytes());
            }
            Object::Date(value) => {
                out.push(0x33);
                out.extend_from_slice(&date::to_seconds(*value).to_be_bytes());
            }
            Object::Data(value) => {
                push_size(&mut out, 0x40, value.len());
                out.extend_from_slice(value);
            }
            Object::String(value) => push_string(&mut out, value),
            Object::Uid(value) => push_uid(&mut out, *value),
            Object::Array(items) => {
                push_size(&mut out, 0xa0, items.len());
                for item in items {
                    push_sized(&mut out, table.number(*item) as u64, ref_size);
                }
            }
            Object::Dict(entries) => {
                push_size(&mut out, 0xd0, entries.len());
                for (key, _) in entries {
                    push_sized(&mut out, table.key_number(key) as u64, ref_size);
                }
                for (_, value) in entries {
                    push_sized(&mut out, table.number(*value) as u64, ref_size);
                }
            }
        }
    }
    let table_offset = out.len() as u64;
    let offset_size = count_to_size(table_offset);
    for offset in offsets {
        push_sized(&mut out, offset, offset_size);
    }
    out.extend_from_slice(&[0; 6]);
    out.extend_from_slice(&[offset_size as u8, ref_size as u8]);
    for field in [count as u64, table.number(plist.root) as u64, table_offset] {
        out.extend_from_slice(&field.to_be_bytes());
    }
    Ok(out)
}

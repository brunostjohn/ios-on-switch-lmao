// SPDX-License-Identifier: GPL-3.0-or-later
//! `plistlib._BinaryPlistParser`: objects are read lazily from the top object, each object-table entry at most once,
//! and a container takes its identity before its children are read, so shared and cyclic references survive.

use std::collections::HashMap;

use super::{Error, Id, Object, Plist, date};

// CPython's recursion limit stops plistlib near 1000 levels; this stops earlier, before Rust's stack would.
const MAX_DEPTH: usize = 512;
const TRAILER: usize = 32;

pub fn read(data: &[u8]) -> Result<Plist, Error> {
    let trailer = &data[data.len().checked_sub(TRAILER).ok_or(Error::Invalid)?..];
    let offset_size = usize::from(trailer[6]);
    let ref_size = usize::from(trailer[7]);
    let [count, top, table] = [8, 16, 24].map(|at| u64::from_be_bytes(trailer[at..at + 8].try_into().unwrap()));
    if i64::try_from(table).is_err() {
        return Err(Error::Invalid);
    }
    let offsets = Cursor { data, at: table }.ints(count, offset_size)?;
    let mut reader = Reader {
        data,
        ref_size,
        cache: vec![None; offsets.len()],
        offsets,
        plist: Plist {
            objects: Vec::new(),
            root: 0,
        },
    };
    reader.plist.root = reader.object(top, 0)?;
    Ok(reader.plist)
}

struct Cursor<'a> {
    data: &'a [u8],
    at: u64,
}

impl<'a> Cursor<'a> {
    /// `_read(n)`: exactly `n` bytes.
    fn take(&mut self, n: u128) -> Result<&'a [u8], Error> {
        if n == 0 {
            return Ok(&[]);
        }
        let start = u128::from(self.at);
        if start + n > self.data.len() as u128 {
            return Err(Error::Invalid);
        }
        self.at += n as u64;
        Ok(&self.data[start as usize..(start + n) as usize])
    }

    /// `fp.read(n)`: up to `n` bytes, fewer at the end of the file.
    fn up_to(&mut self, n: usize) -> &'a [u8] {
        let start = usize::try_from(self.at).map_or(self.data.len(), |at| at.min(self.data.len()));
        let end = start.saturating_add(n).min(self.data.len());
        self.at += (end - start) as u64;
        &self.data[start..end]
    }

    fn byte(&mut self) -> Result<u8, Error> {
        Ok(self.take(1)?[0])
    }

    /// `_read_ints(n, size)`, each saturated to `u64::MAX`, which no table holds.
    fn ints(&mut self, n: u64, size: usize) -> Result<Vec<u64>, Error> {
        let bytes = self.take(u128::from(n) * size as u128)?;
        if size == 0 {
            return Err(Error::Invalid);
        }
        Ok(bytes.chunks(size).map(unsigned).collect())
    }

    /// `_get_size(tokenL)`.
    fn size(&mut self, low: u8) -> Result<u64, Error> {
        if low != 0xf {
            return Ok(u64::from(low));
        }
        let width = 1usize << (self.byte()? & 0x3);
        Ok(unsigned(self.take(width as u128)?))
    }
}

fn unsigned(bytes: &[u8]) -> u64 {
    let split = bytes.len().saturating_sub(8);
    if bytes[..split].iter().any(|b| *b != 0) {
        return u64::MAX;
    }
    bytes[split..].iter().fold(0, |value, b| value << 8 | u64::from(*b))
}

/// `int.from_bytes(bytes, 'big', signed=signed)`.
fn integer(bytes: &[u8], signed: bool) -> Object {
    let negative = signed && bytes.first().is_some_and(|b| b & 0x80 != 0);
    let fill = if negative { 0xff } else { 0 };
    let split = bytes.len().saturating_sub(16);
    let fits = bytes[..split].iter().all(|b| *b == fill)
        && bytes
            .get(split)
            .is_none_or(|b| (b & 0x80 != 0) == negative || bytes.len() < 16);
    if !fits {
        return Object::BigInt;
    }
    let mut wide = [fill; 16];
    wide[16 - (bytes.len() - split)..].copy_from_slice(&bytes[split..]);
    Object::Int(i128::from_be_bytes(wide))
}

/// `struct.unpack('>f')`: a signalling NaN comes back quiet with its payload, as the float-to-double conversion makes
/// it.
fn single(bits: u32) -> f64 {
    let value = f32::from_bits(bits);
    if !value.is_nan() {
        return f64::from(value);
    }
    let sign = u64::from(bits >> 31) << 63;
    f64::from_bits(sign | 0x7ff8_0000_0000_0000 | u64::from(bits & 0x007f_ffff) << 29)
}

struct Reader<'a> {
    data: &'a [u8],
    ref_size: usize,
    offsets: Vec<u64>,
    cache: Vec<Option<Id>>,
    plist: Plist,
}

impl Reader<'_> {
    fn object(&mut self, reference: u64, depth: usize) -> Result<Id, Error> {
        let index = usize::try_from(reference)
            .ok()
            .filter(|index| *index < self.cache.len())
            .ok_or(Error::Invalid)?;
        if let Some(id) = self.cache[index] {
            return Ok(id);
        }
        if depth > MAX_DEPTH {
            return Err(Error::TooDeep(MAX_DEPTH));
        }
        let offset = self.offsets[index];
        if offset >= self.data.len() as u64 {
            return Err(Error::Invalid);
        }
        let mut cursor = Cursor {
            data: self.data,
            at: offset,
        };
        let token = cursor.byte()?;
        let low = token & 0x0f;
        let object = match token {
            0x00 => Object::Null,
            0x08 => Object::Bool(false),
            0x09 => Object::Bool(true),
            0x0f => Object::Data(Vec::new()),
            0x10..=0x1f => integer(cursor.up_to(1 << low), low >= 3),
            0x22 => Object::Real(single(u32::from_be_bytes(cursor.take(4)?.try_into().unwrap()))),
            0x23 => Object::Real(f64::from_be_bytes(cursor.take(8)?.try_into().unwrap())),
            0x33 => {
                let seconds = f64::from_be_bytes(cursor.take(8)?.try_into().unwrap());
                Object::Date(date::from_seconds(seconds).ok_or(Error::Invalid)?)
            }
            0x40..=0x4f => {
                let n = cursor.size(low)?;
                Object::Data(cursor.take(u128::from(n))?.to_vec())
            }
            0x50..=0x5f => {
                let n = cursor.size(low)?;
                let bytes = cursor.take(u128::from(n))?;
                if !bytes.is_ascii() {
                    return Err(Error::Invalid);
                }
                Object::String(String::from_utf8(bytes.to_vec()).expect("ASCII is UTF-8"))
            }
            0x60..=0x6f => {
                let n = cursor.size(low)?;
                let bytes = cursor.take(u128::from(n) * 2)?;
                let units: Vec<u16> = bytes.chunks(2).map(|u| u16::from_be_bytes([u[0], u[1]])).collect();
                Object::String(String::from_utf16(&units).map_err(|_| Error::Invalid)?)
            }
            0x80..=0x8f => {
                let value = cursor.up_to(1 + usize::from(low));
                let split = value.len().saturating_sub(8);
                if value[..split].iter().any(|b| *b != 0) {
                    return Err(Error::Invalid);
                }
                Object::Uid(unsigned(value))
            }
            0xa0..=0xaf => return self.array(index, &mut cursor, low, depth),
            0xd0..=0xdf => return self.dict(index, &mut cursor, low, depth),
            _ => return Err(Error::Invalid),
        };
        Ok(self.remember(index, object))
    }

    fn remember(&mut self, index: usize, object: Object) -> Id {
        let id = self.plist.push(object);
        self.cache[index] = Some(id);
        id
    }

    fn array(&mut self, index: usize, cursor: &mut Cursor, low: u8, depth: usize) -> Result<Id, Error> {
        let n = cursor.size(low)?;
        let refs = cursor.ints(n, self.ref_size)?;
        let id = self.remember(index, Object::Array(Vec::new()));
        let mut items = Vec::with_capacity(refs.len());
        for reference in refs {
            items.push(self.object(reference, depth + 1)?);
        }
        self.plist.objects[id] = Object::Array(items);
        Ok(id)
    }

    /// `result[read(key)] = read(value)`: Python reads the value first, and a repeated key keeps its first place and
    /// takes the last value.
    fn dict(&mut self, index: usize, cursor: &mut Cursor, low: u8, depth: usize) -> Result<Id, Error> {
        let n = cursor.size(low)?;
        let keys = cursor.ints(n, self.ref_size)?;
        let values = cursor.ints(n, self.ref_size)?;
        let id = self.remember(index, Object::Dict(Vec::new()));
        let mut entries: Vec<(String, Id)> = Vec::with_capacity(keys.len());
        let mut places: HashMap<String, usize> = HashMap::with_capacity(keys.len());
        for (key, value) in keys.into_iter().zip(values) {
            let value = self.object(value, depth + 1)?;
            let key = self.object(key, depth + 1)?;
            let key = match &self.plist.objects[key] {
                Object::String(key) => key.clone(),
                Object::Array(_) | Object::Dict(_) => return Err(Error::Invalid),
                _ => return Err(Error::NonStringKey),
            };
            match places.get(&key) {
                Some(&place) => entries[place].1 = value,
                None => {
                    places.insert(key.clone(), entries.len());
                    entries.push((key, value));
                }
            }
        }
        self.plist.objects[id] = Object::Dict(entries);
        Ok(id)
    }
}

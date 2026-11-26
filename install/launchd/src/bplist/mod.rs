// SPDX-License-Identifier: GPL-3.0-or-later
//! Binary property lists read and written as CPython 3.13's `plistlib` does with `fmt=FMT_BINARY` and
//! `sort_keys=False`.
//!
//! `plistlib.loads` makes one Python object per object-table entry, and `dumps` merges scalars by type and value but
//! containers, UIDs and NaNs by identity. So a value here is an arena with one object per entry read, and an object's
//! identity is its [`Id`].

mod date;
mod read;
mod write;
mod xml;

pub use date::Micros;

pub type Id = usize;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("invalid binary plist")]
    Invalid,
    #[error("binary plist nests deeper than {0} containers")]
    TooDeep(usize),
    #[error("binary plist has a dictionary key that is not a string")]
    NonStringKey,
    #[error("integer out of the binary plist range")]
    IntOverflow,
    #[error("unsupported in an imported plist: {0}")]
    Unsupported(&'static str),
}

#[derive(Clone, Debug, PartialEq)]
pub enum Object {
    Null,
    Bool(bool),
    Int(i128),
    /// An integer outside `i128`, which plistlib reads from a 32-byte or wider integer and refuses to write.
    BigInt,
    Real(f64),
    Date(Micros),
    Data(Vec<u8>),
    String(String),
    Uid(u64),
    Array(Vec<Id>),
    Dict(Vec<(String, Id)>),
}

#[derive(Clone, Debug)]
pub struct Plist {
    objects: Vec<Object>,
    root: Id,
}

impl Plist {
    /// `plistlib.loads(data, fmt=FMT_BINARY)`.
    pub fn from_binary(data: &[u8]) -> Result<Plist, Error> {
        read::read(data)
    }

    /// `plistlib.dumps(value, fmt=FMT_BINARY, sort_keys=False)`.
    pub fn to_binary(&self) -> Result<Vec<u8>, Error> {
        write::write(self)
    }

    /// A plist holding `value`, every container of it a new object.
    pub fn from_value(value: &plist::Value) -> Result<Plist, Error> {
        let mut plist = Plist {
            objects: Vec::new(),
            root: 0,
        };
        plist.root = plist.import(value)?;
        Ok(plist)
    }

    /// Adds `value` as new objects, as plistlib's XML reader makes them.
    pub fn import(&mut self, value: &plist::Value) -> Result<Id, Error> {
        xml::import(self, value)
    }

    pub fn root(&self) -> Id {
        self.root
    }

    pub fn object(&self, id: Id) -> &Object {
        &self.objects[id]
    }

    pub fn push(&mut self, object: Object) -> Id {
        self.objects.push(object);
        self.objects.len() - 1
    }

    pub fn len(&self) -> usize {
        self.objects.len()
    }

    pub fn is_empty(&self) -> bool {
        self.objects.is_empty()
    }

    /// `dict.get(key)`, or `None` when `dict` is not a dictionary.
    pub fn get(&self, dict: Id, key: &str) -> Option<Id> {
        match &self.objects[dict] {
            Object::Dict(entries) => entries.iter().find(|(k, _)| k == key).map(|(_, v)| *v),
            _ => None,
        }
    }

    /// `dict[key] = value`: a key already there keeps its place and takes the new value.
    pub fn insert(&mut self, dict: Id, key: &str, value: Id) {
        let Object::Dict(entries) = &mut self.objects[dict] else {
            panic!("insert into object {dict}, which is not a dictionary");
        };
        match entries.iter_mut().find(|(k, _)| k == key) {
            Some(entry) => entry.1 = value,
            None => entries.push((key.to_owned(), value)),
        }
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later
//! A kernelcache's `__PRELINK_INFO`: the XML `OSUnserializeXML` reads, as `native_prelink.unserialize_xml` reads it and
//! as `emit_prelink.to_xml` with ElementTree writes it.

mod read;
mod write;

pub(crate) use read::{unserialize, unserialize_plist};
pub(crate) use write::serialize;

use crate::error::{Result, refused};

#[derive(Clone, Debug, PartialEq)]
pub(crate) enum Value {
    Dict(Dict),
    Array(Vec<Value>),
    String(String),
    Integer(i128),
    Data(Vec<u8>),
    Bool(bool),
}

/// A dictionary in insertion order, as a Python dict keeps it.
#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct Dict(pub Vec<(String, Value)>);

impl Dict {
    pub fn get(&self, key: &str) -> Option<&Value> {
        self.0.iter().find(|(k, _)| k == key).map(|(_, v)| v)
    }

    pub fn get_mut(&mut self, key: &str) -> Option<&mut Value> {
        self.0.iter_mut().find(|(k, _)| k == key).map(|(_, v)| v)
    }

    pub fn contains(&self, key: &str) -> bool {
        self.get(key).is_some()
    }

    /// Python's `dict[key] = value`: a new key goes last, an existing one keeps its place.
    pub fn set(&mut self, key: &str, value: Value) {
        match self.get_mut(key) {
            Some(slot) => *slot = value,
            None => self.0.push((key.to_owned(), value)),
        }
    }

    pub fn remove(&mut self, key: &str) -> Option<Value> {
        let at = self.0.iter().position(|(k, _)| k == key)?;
        Some(self.0.remove(at).1)
    }

    pub fn keys(&self) -> impl Iterator<Item = &str> {
        self.0.iter().map(|(k, _)| k.as_str())
    }

    pub fn dict(&self, key: &str) -> Option<&Dict> {
        match self.get(key) {
            Some(Value::Dict(d)) => Some(d),
            _ => None,
        }
    }

    pub fn dict_mut(&mut self, key: &str) -> Option<&mut Dict> {
        match self.get_mut(key) {
            Some(Value::Dict(d)) => Some(d),
            _ => None,
        }
    }

    pub fn string(&self, key: &str) -> Result<&str> {
        match self.get(key) {
            Some(Value::String(s)) => Ok(s),
            _ => refused(format!("Prelink info has no string {key}")),
        }
    }

    /// Python's `dict.get(key) is True`.
    pub fn is_true(&self, key: &str) -> bool {
        self.get(key) == Some(&Value::Bool(true))
    }
}

impl Value {
    /// Python's truth value of what `unserialize` returns.
    pub fn truthy(&self) -> bool {
        match self {
            Value::Dict(d) => !d.0.is_empty(),
            Value::Array(a) => !a.is_empty(),
            Value::String(s) => !s.is_empty(),
            Value::Integer(v) => *v != 0,
            Value::Data(d) => !d.is_empty(),
            Value::Bool(b) => *b,
        }
    }
}

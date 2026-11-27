// SPDX-License-Identifier: GPL-3.0-or-later
use plist::Value;

use super::{Error, Id, Object, Plist};

/// What plistlib's XML reader would make of `value`: every element a new object. Dates and UIDs are refused rather
/// than matched to `_date_from_string`, which no input here needs.
pub fn import(plist: &mut Plist, value: &Value) -> Result<Id, Error> {
    let object = match value {
        Value::Array(items) => Object::Array(items.iter().map(|item| import(plist, item)).collect::<Result<_, _>>()?),
        Value::Dictionary(dict) => {
            let mut entries = Vec::with_capacity(dict.len());
            for (key, value) in dict {
                entries.push((key.clone(), import(plist, value)?));
            }
            Object::Dict(entries)
        }
        Value::Boolean(value) => Object::Bool(*value),
        Value::Data(value) => Object::Data(value.clone()),
        Value::Integer(value) => Object::Int(
            value
                .as_signed()
                .map(i128::from)
                .or_else(|| value.as_unsigned().map(i128::from))
                .ok_or(Error::Unsupported("an integer"))?,
        ),
        Value::Real(value) => Object::Real(*value),
        Value::String(value) => Object::String(value.clone()),
        Value::Date(_) => return Err(Error::Unsupported("a date")),
        Value::Uid(_) => return Err(Error::Unsupported("a UID")),
        _ => return Err(Error::Unsupported("an unknown value")),
    };
    Ok(plist.push(object))
}

// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace switch_seed {

struct JsonValue {
    enum class Kind {
        Null,
        Bool,
        Int,
        Uint,
        String,
        Array,
        Object,
    };

    Kind kind = Kind::Null;
    bool boolean = false;
    int64_t integer = 0;
    uint64_t uinteger = 0;
    std::string text;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;

    static JsonValue makeNull()
    {
        return {};
    }

    static JsonValue makeBool(bool v)
    {
        JsonValue out;
        out.kind = Kind::Bool;
        out.boolean = v;
        return out;
    }

    static JsonValue makeInt(int64_t v)
    {
        JsonValue out;
        out.kind = Kind::Int;
        out.integer = v;
        return out;
    }

    static JsonValue makeUint(uint64_t v)
    {
        JsonValue out;
        out.kind = Kind::Uint;
        out.uinteger = v;
        return out;
    }

    static JsonValue makeString(std::string v)
    {
        JsonValue out;
        out.kind = Kind::String;
        out.text = std::move(v);
        return out;
    }

    static JsonValue makeArray(std::vector<JsonValue> v = {})
    {
        JsonValue out;
        out.kind = Kind::Array;
        out.array = std::move(v);
        return out;
    }

    static JsonValue makeObject(std::map<std::string, JsonValue> v = {})
    {
        JsonValue out;
        out.kind = Kind::Object;
        out.object = std::move(v);
        return out;
    }

    bool isString() const
    {
        return kind == Kind::String;
    }

    bool isObject() const
    {
        return kind == Kind::Object;
    }

    bool isArray() const
    {
        return kind == Kind::Array;
    }

    bool equalsUint(uint64_t v) const
    {
        return (kind == Kind::Uint && uinteger == v) || (kind == Kind::Int && integer >= 0 && uint64_t(integer) == v);
    }

    const JsonValue *field(const std::string &name) const
    {
        if (kind != Kind::Object)
            return nullptr;
        const auto it = object.find(name);
        return it == object.end() ? nullptr : &it->second;
    }
};

std::string jsonSerialize(const JsonValue &value);
bool jsonParse(const std::string &text, JsonValue &out);

}  // namespace switch_seed

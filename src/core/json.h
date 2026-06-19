#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace anyadance::json {

enum class Type { Null, Bool, Number, String, Array, Object };

// A small order-preserving JSON value used to read, edit, and rewrite SteamVR
// configuration files (openvrpaths.vrpath, steamvr.vrsettings) without losing
// unrelated keys. Not a general-purpose library; just enough for that job.
struct Value {
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    static Value Bool(bool v);
    static Value Number(double v);
    static Value String(std::string v);
    static Value Array();
    static Value Object();

    bool IsObject() const { return type == Type::Object; }
    bool IsArray() const { return type == Type::Array; }

    // Object access; null if this is not an object or the key is absent.
    Value* Find(const std::string& key);
    const Value* Find(const std::string& key) const;
    // Upsert into an object, preserving key order.
    void Set(const std::string& key, Value value);
};

std::optional<Value> Parse(const std::string& text);
std::string Serialize(const Value& value);

} // namespace anyadance::json

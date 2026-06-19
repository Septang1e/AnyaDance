#include "core/json.h"

#include <cmath>
#include <cstdio>

namespace anyadance::json {
namespace {

struct Parser {
    const std::string& text;
    std::size_t pos = 0;
    bool ok = true;

    explicit Parser(const std::string& t) : text(t) {}

    void SkipWhitespace() {
        while (pos < text.size()) {
            const char c = text[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos;
            } else {
                break;
            }
        }
    }

    char Peek() { return pos < text.size() ? text[pos] : '\0'; }

    bool Consume(char expected) {
        if (Peek() == expected) {
            ++pos;
            return true;
        }
        ok = false;
        return false;
    }

    Value ParseValue() {
        SkipWhitespace();
        if (!ok || pos >= text.size()) {
            ok = false;
            return {};
        }
        const char c = text[pos];
        switch (c) {
        case '{': return ParseObject();
        case '[': return ParseArray();
        case '"': return Value::String(ParseString());
        case 't':
        case 'f': return ParseBool();
        case 'n': return ParseNull();
        default: return ParseNumber();
        }
    }

    std::string ParseString() {
        std::string result;
        if (!Consume('"')) {
            return result;
        }
        while (pos < text.size()) {
            const char c = text[pos++];
            if (c == '"') {
                return result;
            }
            if (c == '\\') {
                if (pos >= text.size()) {
                    break;
                }
                const char esc = text[pos++];
                switch (esc) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u': {
                    if (pos + 4 > text.size()) { ok = false; return result; }
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = text[pos++];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                        else { ok = false; return result; }
                    }
                    // Encode the basic-plane code point as UTF-8 (sufficient for
                    // SteamVR config files, which avoid surrogate pairs).
                    if (code < 0x80) {
                        result.push_back(static_cast<char>(code));
                    } else if (code < 0x800) {
                        result.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    } else {
                        result.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    break;
                }
                default: ok = false; return result;
                }
            } else {
                result.push_back(c);
            }
        }
        ok = false;
        return result;
    }

    Value ParseObject() {
        Value value = Value::Object();
        Consume('{');
        SkipWhitespace();
        if (Peek() == '}') { ++pos; return value; }
        while (ok) {
            SkipWhitespace();
            if (Peek() != '"') { ok = false; break; }
            const std::string key = ParseString();
            SkipWhitespace();
            if (!Consume(':')) { break; }
            Value child = ParseValue();
            if (!ok) { break; }
            value.object.emplace_back(key, std::move(child));
            SkipWhitespace();
            const char c = Peek();
            if (c == ',') { ++pos; continue; }
            if (c == '}') { ++pos; return value; }
            ok = false;
            break;
        }
        return value;
    }

    Value ParseArray() {
        Value value = Value::Array();
        Consume('[');
        SkipWhitespace();
        if (Peek() == ']') { ++pos; return value; }
        while (ok) {
            Value child = ParseValue();
            if (!ok) { break; }
            value.array.push_back(std::move(child));
            SkipWhitespace();
            const char c = Peek();
            if (c == ',') { ++pos; continue; }
            if (c == ']') { ++pos; return value; }
            ok = false;
            break;
        }
        return value;
    }

    Value ParseBool() {
        if (text.compare(pos, 4, "true") == 0) { pos += 4; return Value::Bool(true); }
        if (text.compare(pos, 5, "false") == 0) { pos += 5; return Value::Bool(false); }
        ok = false;
        return {};
    }

    Value ParseNull() {
        if (text.compare(pos, 4, "null") == 0) { pos += 4; return {}; }
        ok = false;
        return {};
    }

    Value ParseNumber() {
        const std::size_t start = pos;
        while (pos < text.size()) {
            const char c = text[pos];
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') {
                ++pos;
            } else {
                break;
            }
        }
        if (pos == start) { ok = false; return {}; }
        try {
            return Value::Number(std::stod(text.substr(start, pos - start)));
        } catch (...) {
            ok = false;
            return {};
        }
    }
};

void AppendEscaped(std::string& out, const std::string& s) {
    out.push_back('"');
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    out.push_back('"');
}

void AppendNumber(std::string& out, double n) {
    if (std::isfinite(n) && std::floor(n) == n && std::fabs(n) < 1e15) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(n));
        out += buf;
    } else {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.17g", n);
        out += buf;
    }
}

void Indent(std::string& out, int depth) {
    out.append(static_cast<std::size_t>(depth) * 3, ' ');
}

void SerializeInto(std::string& out, const Value& value, int depth) {
    switch (value.type) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += value.boolean ? "true" : "false"; break;
    case Type::Number: AppendNumber(out, value.number); break;
    case Type::String: AppendEscaped(out, value.string); break;
    case Type::Array:
        if (value.array.empty()) { out += "[]"; break; }
        out += "[\n";
        for (std::size_t i = 0; i < value.array.size(); ++i) {
            Indent(out, depth + 1);
            SerializeInto(out, value.array[i], depth + 1);
            out += (i + 1 < value.array.size()) ? ",\n" : "\n";
        }
        Indent(out, depth);
        out += "]";
        break;
    case Type::Object:
        if (value.object.empty()) { out += "{}"; break; }
        out += "{\n";
        for (std::size_t i = 0; i < value.object.size(); ++i) {
            Indent(out, depth + 1);
            AppendEscaped(out, value.object[i].first);
            out += " : ";
            SerializeInto(out, value.object[i].second, depth + 1);
            out += (i + 1 < value.object.size()) ? ",\n" : "\n";
        }
        Indent(out, depth);
        out += "}";
        break;
    }
}

} // namespace

Value Value::Bool(bool v) { Value x; x.type = Type::Bool; x.boolean = v; return x; }
Value Value::Number(double v) { Value x; x.type = Type::Number; x.number = v; return x; }
Value Value::String(std::string v) { Value x; x.type = Type::String; x.string = std::move(v); return x; }
Value Value::Array() { Value x; x.type = Type::Array; return x; }
Value Value::Object() { Value x; x.type = Type::Object; return x; }

Value* Value::Find(const std::string& key) {
    if (type != Type::Object) { return nullptr; }
    for (auto& entry : object) {
        if (entry.first == key) { return &entry.second; }
    }
    return nullptr;
}

const Value* Value::Find(const std::string& key) const {
    if (type != Type::Object) { return nullptr; }
    for (const auto& entry : object) {
        if (entry.first == key) { return &entry.second; }
    }
    return nullptr;
}

void Value::Set(const std::string& key, Value value) {
    if (type != Type::Object) {
        type = Type::Object;
        object.clear();
    }
    for (auto& entry : object) {
        if (entry.first == key) {
            entry.second = std::move(value);
            return;
        }
    }
    object.emplace_back(key, std::move(value));
}

std::optional<Value> Parse(const std::string& text) {
    Parser parser(text);
    Value value = parser.ParseValue();
    if (!parser.ok) { return std::nullopt; }
    parser.SkipWhitespace();
    if (parser.pos != text.size()) { return std::nullopt; }
    return value;
}

std::string Serialize(const Value& value) {
    std::string out;
    SerializeInto(out, value, 0);
    out.push_back('\n');
    return out;
}

} // namespace anyadance::json

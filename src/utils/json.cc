#include "json.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

namespace aura::json {

namespace {

const Value kNullValue;  // 共享的 null 哨兵值

// JSON 字符串转义
std::string EscapeString(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

}  // namespace

const Value& Null() { return kNullValue; }

Value Value::Parse(const std::string& text, std::string* error) {
    Parser p{text};
    p.SkipWs();
    if (p.pos >= p.s.size()) {
        if (error) *error = "empty input";
        return Value();
    }
    Value v = p.ParseValue();
    if (!p.ok) {
        if (error) *error = p.error;
        return Value();
    }
    p.SkipWs();
    if (p.pos != p.s.size()) {
        if (error) *error = "trailing content at position " + std::to_string(p.pos);
        return Value();
    }
    return v;
}

std::string Value::Dump() const {
    switch (type_) {
        case Type::kNull: return "null";
        case Type::kBool: return bool_ ? "true" : "false";
        case Type::kNumber: {
            char buf[32];
            if (num_ == static_cast<int64_t>(num_)) {
                snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(num_));
            } else {
                snprintf(buf, sizeof(buf), "%g", num_);
            }
            return buf;
        }
        case Type::kString: return "\"" + EscapeString(str_) + "\"";
        case Type::kArray: {
            std::string out = "[";
            for (size_t i = 0; i < arr_.size(); ++i) {
                if (i) out += ",";
                out += arr_[i].Dump();
            }
            return out + "]";
        }
        case Type::kObject: {
            std::string out = "{";
            bool first = true;
            for (const auto& [k, v] : obj_) {
                if (!first) out += ",";
                first = false;
                out += "\"" + EscapeString(k) + "\":" + v.Dump();
            }
            return out + "}";
        }
    }
    return "null";
}

const Value& Value::operator[](size_t index) const {
    if (type_ != Type::kArray || index >= arr_.size()) return kNullValue;
    return arr_[index];
}

const Value& Value::operator[](const std::string& key) const {
    auto it = obj_.find(key);
    if (type_ != Type::kObject || it == obj_.end()) return kNullValue;
    return it->second;
}

bool Value::Has(const std::string& key) const {
    return type_ == Type::kObject && obj_.count(key) != 0;
}

int Value::GetInt(const std::string& key, int def) const {
    const Value& v = (*this)[key];
    return v.is_number() ? v.as_int() : def;
}

double Value::GetNumber(const std::string& key, double def) const {
    const Value& v = (*this)[key];
    return v.is_number() ? v.as_number() : def;
}

bool Value::GetBool(const std::string& key, bool def) const {
    const Value& v = (*this)[key];
    return v.is_bool() ? v.as_bool() : def;
}

std::string Value::GetString(const std::string& key, const std::string& def) const {
    const Value& v = (*this)[key];
    return v.is_string() ? v.as_string() : def;
}

void Value::Push(Value v) {
    if (type_ != Type::kArray) return;
    arr_.push_back(std::move(v));
}

void Value::Set(const std::string& key, Value v) {
    if (type_ != Type::kObject) return;
    obj_[key] = std::move(v);
}

// ---------------- 解析器 ----------------

void Value::Parser::SkipWs() {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r')) {
        ++pos;
    }
}

bool Value::Parser::Consume(char c) {
    if (pos < s.size() && s[pos] == c) {
        ++pos;
        return true;
    }
    return false;
}

Value Value::Parser::ParseValue() {
    SkipWs();
    if (pos >= s.size()) {
        ok = false;
        error = "unexpected end of input";
        return Value();
    }
    char c = s[pos];
    if (c == '{') return ParseObject();
    if (c == '[') return ParseArray();
    if (c == '"') return ParseString();
    if (c == 't' || c == 'f') {
        std::string lit = (c == 't') ? "true" : "false";
        if (s.compare(pos, lit.size(), lit) == 0) {
            pos += lit.size();
            return Value(c == 't');
        }
        ok = false;
        error = "invalid literal at position " + std::to_string(pos);
        return Value();
    }
    if (c == 'n') {
        if (s.compare(pos, 4, "null") == 0) {
            pos += 4;
            return Value();
        }
        ok = false;
        error = "invalid literal at position " + std::to_string(pos);
        return Value();
    }
    return ParseNumber();
}

Value Value::Parser::ParseString() {
    // 进入时 pos 指向 '"'
    ++pos;
    std::string out;
    while (pos < s.size()) {
        char c = s[pos++];
        if (c == '"') return Value(std::move(out));
        if (c == '\\') {
            if (pos >= s.size()) break;
            char e = s[pos++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u':  // 简化：\uXXXX 原样保留前 4 个字符
                    if (pos + 4 <= s.size()) {
                        out += "\\u" + s.substr(pos, 4);
                        pos += 4;
                    }
                    break;
                default: out += e; break;
            }
        } else {
            out += c;
        }
    }
    ok = false;
    error = "unterminated string";
    return Value();
}

Value Value::Parser::ParseNumber() {
    size_t start = pos;
    if (pos < s.size() && (s[pos] == '-' || s[pos] == '+')) ++pos;
    bool has_digit = false;
    while (pos < s.size() && (isdigit(static_cast<unsigned char>(s[pos])) || s[pos] == '.'
                              || s[pos] == 'e' || s[pos] == 'E' || s[pos] == '-' || s[pos] == '+')) {
        if (isdigit(static_cast<unsigned char>(s[pos]))) has_digit = true;
        ++pos;
    }
    if (!has_digit) {
        ok = false;
        error = "invalid number at position " + std::to_string(start);
        return Value();
    }
    return Value(strtod(s.substr(start, pos - start).c_str(), nullptr));
}

Value Value::Parser::ParseArray() {
    ++pos;  // '['
    Value arr(Type::kArray);
    SkipWs();
    if (Consume(']')) return arr;
    while (true) {
        arr.Push(ParseValue());
        if (!ok) return Value();
        SkipWs();
        if (Consume(']')) return arr;
        if (!Consume(',')) {
            ok = false;
            error = "expected ',' or ']' in array at position " + std::to_string(pos);
            return Value();
        }
    }
}

Value Value::Parser::ParseObject() {
    ++pos;  // '{'
    Value obj(Type::kObject);
    SkipWs();
    if (Consume('}')) return obj;
    while (true) {
        SkipWs();
        if (pos >= s.size() || s[pos] != '"') {
            ok = false;
            error = "expected string key in object at position " + std::to_string(pos);
            return Value();
        }
        Value key = ParseString();
        if (!ok) return Value();
        SkipWs();
        if (!Consume(':')) {
            ok = false;
            error = "expected ':' at position " + std::to_string(pos);
            return Value();
        }
        obj.Set(key.as_string(), ParseValue());
        if (!ok) return Value();
        SkipWs();
        if (Consume('}')) return obj;
        if (!Consume(',')) {
            ok = false;
            error = "expected ',' or '}' in object at position " + std::to_string(pos);
            return Value();
        }
    }
}

}  // namespace aura::json

// rtltcp input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// A minimal JSON reader and writer, just enough for the FernSDR module
// protocol (API 1): objects, arrays, strings, numbers, booleans and null.
// The strings carry no lone surrogates on output, and unknown escapes are
// refused on input, as a receiver's own parser would refuse them.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<Value> array;
    std::map<std::string, Value> object;

    static Value make_null() { return Value(); }
    static Value make_bool(bool b) { Value v; v.type = Bool; v.boolean = b; return v; }
    static Value make_number(double n) { Value v; v.type = Number; v.number = n; return v; }
    static Value make_string(std::string s) { Value v; v.type = String; v.string = std::move(s); return v; }
    static Value make_array() { Value v; v.type = Array; return v; }
    static Value make_object() { Value v; v.type = Object; return v; }

    bool is_object() const { return type == Object; }
    bool is_array() const { return type == Array; }
    bool is_string() const { return type == String; }
    bool is_number() const { return type == Number; }
    bool is_bool() const { return type == Bool; }

    bool has(const std::string& key) const { return type == Object && object.count(key) != 0; }
    const Value& get(const std::string& key) const {
        static const Value nothing;
        if (type != Object) return nothing;
        auto it = object.find(key);
        return it == object.end() ? nothing : it->second;
    }
    void set(const std::string& key, Value v) { object[key] = std::move(v); }
    void push(Value v) { array.push_back(std::move(v)); }

    std::string as_string(const std::string& fallback = "") const {
        return type == String ? string : fallback;
    }
    double as_number(double fallback = 0.0) const { return type == Number ? number : fallback; }
    int64_t as_int(int64_t fallback = 0) const {
        return type == Number ? static_cast<int64_t>(number) : fallback;
    }
    bool as_bool(bool fallback = false) const { return type == Bool ? boolean : fallback; }
};

// --- parser ---------------------------------------------------------------

class Parser {
public:
    explicit Parser(const std::string& text) : p_(text.data()), end_(text.data() + text.size()) {}

    bool parse(Value& out) {
        skip_ws();
        return parse_value(out) && trailing_is_ws();
    }

    // Where parsing stopped (for diagnostics).
    const char* position() const { return p_; }

    bool trailing_is_ws() {
        skip_ws();
        return p_ == end_;
    }

private:
    const char* p_;
    const char* end_;

    void skip_ws() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
    }

    bool parse_value(Value& out) {
        skip_ws();
        if (p_ >= end_) return false;
        switch (*p_) {
            case '{': return parse_object(out);
            case '[': return parse_array(out);
            case '"': { std::string s; if (!parse_string(s)) return false; out = Value::make_string(std::move(s)); return true; }
            case 't': return literal("true", Value::make_bool(true), out);
            case 'f': return literal("false", Value::make_bool(false), out);
            case 'n': return literal("null", Value::make_null(), out);
            default: return parse_number(out);
        }
    }

    bool literal(const char* word, Value made, Value& out) {
        size_t n = strlen(word);
        if (static_cast<size_t>(end_ - p_) < n || memcmp(p_, word, n) != 0) return false;
        p_ += n;
        out = std::move(made);
        return true;
    }

    bool parse_object(Value& out) {
        ++p_;  // '{'
        out = Value::make_object();
        skip_ws();
        if (p_ < end_ && *p_ == '}') { ++p_; return true; }
        while (true) {
            skip_ws();
            if (p_ >= end_ || *p_ != '"') return false;
            std::string key;
            if (!parse_string(key)) return false;
            skip_ws();
            if (p_ >= end_ || *p_ != ':') return false;
            ++p_;
            Value v;
            if (!parse_value(v)) return false;
            out.set(key, std::move(v));
            skip_ws();
            if (p_ >= end_) return false;
            if (*p_ == ',') { ++p_; continue; }
            if (*p_ == '}') { ++p_; return true; }
            return false;
        }
    }

    bool parse_array(Value& out) {
        ++p_;  // '['
        out = Value::make_array();
        skip_ws();
        if (p_ < end_ && *p_ == ']') { ++p_; return true; }
        while (true) {
            Value v;
            if (!parse_value(v)) return false;
            out.push(std::move(v));
            skip_ws();
            if (p_ >= end_) return false;
            if (*p_ == ',') { ++p_; continue; }
            if (*p_ == ']') { ++p_; return true; }
            return false;
        }
    }

    bool parse_string(std::string& out) {
        if (p_ >= end_ || *p_ != '"') return false;
        ++p_;
        out.clear();
        while (true) {
            if (p_ >= end_) return false;
            unsigned char c = static_cast<unsigned char>(*p_);
            if (c == '"') { ++p_; return true; }
            if (c < 0x20) return false;  // control character in a string
            if (c == '\\') {
                ++p_;
                if (p_ >= end_) return false;
                char e = *p_++;
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (!parse_unicode_escape(out)) return false;
                        break;
                    }
                    default: return false;
                }
            } else {
                out.push_back(static_cast<char>(c));
                ++p_;
            }
        }
    }

    static int hex_digit(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool read_hex4(uint32_t& v) {
        if (static_cast<size_t>(end_ - p_) < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            int d = hex_digit(*p_);
            if (d < 0) return false;
            v = (v << 4) | static_cast<uint32_t>(d);
            ++p_;
        }
        return true;
    }

    static void append_utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parse_unicode_escape(std::string& out) {
        uint32_t first = 0;
        if (!read_hex4(first)) return false;
        if (first >= 0xD800 && first <= 0xDBFF) {
            // High surrogate; a low surrogate must follow.
            if (static_cast<size_t>(end_ - p_) < 2 || p_[0] != '\\' || p_[1] != 'u') return false;
            p_ += 2;
            uint32_t second = 0;
            if (!read_hex4(second)) return false;
            if (second < 0xDC00 || second > 0xDFFF) return false;
            uint32_t cp = 0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00);
            append_utf8(out, cp);
        } else if (first >= 0xDC00 && first <= 0xDFFF) {
            return false;  // lone low surrogate
        } else {
            append_utf8(out, first);
        }
        return true;
    }

    bool parse_number(Value& out) {
        const char* start = p_;
        if (p_ < end_ && *p_ == '-') ++p_;
        while (p_ < end_ && ((*p_ >= '0' && *p_ <= '9') || *p_ == '.' || *p_ == 'e' ||
                             *p_ == 'E' || *p_ == '+' || *p_ == '-'))
            ++p_;
        if (p_ == start) return false;
        std::string token(start, p_);
        errno = 0;
        char* endptr = nullptr;
        double v = strtod(token.c_str(), &endptr);
        if (endptr != token.c_str() + token.size()) return false;
        if (!std::isfinite(v)) return false;  // strtod takes inf/nan; FernSDR does not
        out = Value::make_number(v);
        return true;
    }
};

// --- writer ---------------------------------------------------------------

inline void escape_string(const std::string& s, std::string& out) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

inline void print_number(double v, std::string& out) {
    if (v == std::floor(v) && v >= -9007199254740992.0 && v <= 9007199254740992.0) {
        char buf[32];
        snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
        out += buf;
    } else {
        char buf[64];
        snprintf(buf, sizeof buf, "%.15g", v);
        out += buf;
    }
}

inline void serialize(const Value& v, std::string& out) {
    switch (v.type) {
        case Value::Null: out += "null"; break;
        case Value::Bool: out += v.boolean ? "true" : "false"; break;
        case Value::Number: print_number(v.number, out); break;
        case Value::String: escape_string(v.string, out); break;
        case Value::Array: {
            out.push_back('[');
            for (size_t i = 0; i < v.array.size(); ++i) {
                if (i) out.push_back(',');
                serialize(v.array[i], out);
            }
            out.push_back(']');
            break;
        }
        case Value::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto& [key, value] : v.object) {
                if (!first) out.push_back(',');
                first = false;
                escape_string(key, out);
                out.push_back(':');
                serialize(value, out);
            }
            out.push_back('}');
            break;
        }
    }
}

inline std::string dump(const Value& v) {
    std::string out;
    serialize(v, out);
    return out;
}

}  // namespace json

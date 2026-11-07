#include "json.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "error.h"

namespace xm {

namespace {

struct Parser {
    const std::string& t;
    size_t p = 0;
    int depth = 0;

    // The message carries a short excerpt of the text around the failure, so a malformed camera reply can
    // be diagnosed from the error alone. Control characters are shown as \xNN. (The excerpt is part of the reply.)
    [[noreturn]] void fail(const char* what) const {
        size_t a = p > 30 ? p - 30 : 0, b = std::min(t.size(), p + 30);
        std::string ex;
        for (size_t i = a; i < b; i++) {
            unsigned char c = (unsigned char)t[i];
            if (c >= 0x20 && c < 0x7f) ex.push_back((char)c);
            else { char h[8]; snprintf(h, sizeof h, "\\x%02x", c); ex += h; }
        }
        throw Error(Errc::Protocol, std::string("JSON: ") + what + " at byte " + std::to_string(p) + " of " +
                                        std::to_string(t.size()) + ", near: [" + ex + "]");
    }
    void ws() { while (p < t.size() && (t[p] == ' ' || t[p] == '\n' || t[p] == '\r' || t[p] == '\t')) p++; }
    bool lit(const char* s) {
        size_t n = 0;
        while (s[n]) n++;
        if (t.compare(p, n, s) == 0) { p += n; return true; }
        return false;
    }
    static void put_utf8(std::string& o, uint32_t c) {
        if (c < 0x80) o.push_back((char)c);
        else if (c < 0x800) { o.push_back((char)(0xC0 | (c >> 6))); o.push_back((char)(0x80 | (c & 0x3F))); }
        else if (c < 0x10000) {
            o.push_back((char)(0xE0 | (c >> 12))); o.push_back((char)(0x80 | ((c >> 6) & 0x3F))); o.push_back((char)(0x80 | (c & 0x3F)));
        } else {
            o.push_back((char)(0xF0 | (c >> 18))); o.push_back((char)(0x80 | ((c >> 12) & 0x3F)));
            o.push_back((char)(0x80 | ((c >> 6) & 0x3F))); o.push_back((char)(0x80 | (c & 0x3F)));
        }
    }
    uint32_t hex4() {
        if (p + 4 > t.size()) fail("short \\u escape");
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) {
            char c = t[p++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else fail("bad \\u escape");
        }
        return v;
    }
    std::string string() {
        p++;  // opening quote
        std::string o;
        for (;;) {
            if (p >= t.size()) fail("unterminated string");
            char c = t[p++];
            if (c == '"') return o;
            if (c != '\\') { o.push_back(c); continue; }
            if (p >= t.size()) fail("unterminated escape");
            char e = t[p++];
            switch (e) {
                case '"': o.push_back('"'); break;
                case '\\': o.push_back('\\'); break;
                case '/': o.push_back('/'); break;
                case 'b': o.push_back('\b'); break;
                case 'f': o.push_back('\f'); break;
                case 'n': o.push_back('\n'); break;
                case 'r': o.push_back('\r'); break;
                case 't': o.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = hex4();
                    if (cp >= 0xD800 && cp < 0xDC00 && t.compare(p, 2, "\\u") == 0) {
                        p += 2;
                        uint32_t lo = hex4();
                        if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else fail("bad surrogate pair");
                    }
                    put_utf8(o, cp);
                    break;
                }
                default: fail("bad escape");
            }
        }
    }
    Json value() {
        if (++depth > 64) fail("nesting too deep");
        ws();
        if (p >= t.size()) fail("unexpected end");
        Json r;
        char c = t[p];
        if (c == '{') {
            p++;
            r = Json::object();
            ws();
            if (p < t.size() && t[p] == '}') p++;
            else for (;;) {
                ws();
                if (p >= t.size() || t[p] != '"') fail("object key expected");
                std::string k = string();
                ws();
                if (p >= t.size() || t[p] != ':') fail("':' expected");
                p++;
                r.set(k, value());
                ws();
                if (p < t.size() && t[p] == ',') { p++; continue; }
                if (p < t.size() && t[p] == '}') { p++; break; }
                fail("',' or '}' expected");
            }
        } else if (c == '[') {
            p++;
            r = Json::array();
            ws();
            if (p < t.size() && t[p] == ']') p++;
            else for (;;) {
                r.push(value());
                ws();
                if (p < t.size() && t[p] == ',') { p++; continue; }
                if (p < t.size() && t[p] == ']') { p++; break; }
                fail("',' or ']' expected");
            }
        } else if (c == '"') {
            r = Json(string());
        } else if (lit("true")) r = Json(true);
        else if (lit("false")) r = Json(false);
        else if (lit("null")) r = Json();
        else {
            // std::from_chars is locale-independent. strtod() is not: under a comma-decimal locale (ru, uk, de ...)
            // it treats the comma in  {"a" : 30, "b" : 1}  as part of the number.
            const char* b = t.data() + p;
            const char* end = t.data() + t.size();
            if (b < end && *b == '+') fail("value expected");  // JSON has no leading '+'
            double d = 0;
            auto res = std::from_chars(b, end, d);
            if (res.ec != std::errc() || res.ptr == b) fail("value expected");
            p += (size_t)(res.ptr - b);
            r = Json(d);
        }
        depth--;
        return r;
    }
};

const Json kNull;

}  // namespace

Json Json::parse(const std::string& text) {
    Parser ps{text};
    Json v = ps.value();
    ps.ws();
    if (ps.p != text.size()) ps.fail("trailing data");
    return v;
}

std::string Json::quote(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o.push_back((char)c);
        }
    }
    return o + "\"";
}

std::string Json::dump() const {
    switch (type_) {
        case Type::Null: return "null";
        case Type::Bool: return b_ ? "true" : "false";
        case Type::Number: {
            char b[48];
            if (n_ == std::floor(n_) && std::fabs(n_) < 9e15) {
                snprintf(b, sizeof b, "%lld", (long long)n_);  // integers: no decimal separator involved
                return b;
            }
            auto res = std::to_chars(b, b + sizeof b, n_);  // locale-independent shortest round-trip form
            return std::string(b, res.ptr);
        }
        case Type::String: return quote(s_);
        case Type::Array: {
            std::string o = "[";
            for (size_t i = 0; i < arr_.size(); i++) { if (i) o += ","; o += arr_[i].dump(); }
            return o + "]";
        }
        case Type::Object: {
            std::string o = "{";
            for (size_t i = 0; i < obj_.size(); i++) { if (i) o += ","; o += quote(obj_[i].first) + ":" + obj_[i].second.dump(); }
            return o + "}";
        }
    }
    return "null";
}

const Json& Json::operator[](const std::string& key) const {
    if (type_ == Type::Object)
        for (auto& kv : obj_) if (kv.first == key) return kv.second;
    return kNull;
}

bool Json::contains(const std::string& key) const {
    if (type_ == Type::Object)
        for (auto& kv : obj_) if (kv.first == key) return true;
    return false;
}

Json& Json::set(const std::string& key, Json v) {
    if (type_ != Type::Object) throw Error(Errc::Protocol, "Json::set on a non-object");
    for (auto& kv : obj_) if (kv.first == key) { kv.second = std::move(v); return *this; }
    obj_.emplace_back(key, std::move(v));
    return *this;
}

void Json::push(Json v) {
    if (type_ != Type::Array) throw Error(Errc::Protocol, "Json::push on a non-array");
    arr_.push_back(std::move(v));
}

std::string Json::str(const std::string& def) const {
    if (type_ == Type::String) return s_;
    if (type_ == Type::Number || type_ == Type::Bool) return dump();
    return def;
}

int64_t Json::integer(int64_t def) const {
    if (type_ == Type::Number) return (int64_t)n_;
    if (type_ == Type::Bool) return b_ ? 1 : 0;
    if (type_ == Type::String && !s_.empty()) {
        char* e = nullptr;
        long long v = strtoll(s_.c_str(), &e, 0);  // base 0: "0x.." hex or decimal
        if (e && *e == '\0') return v;
    }
    return def;
}

bool Json::boolean(bool def) const {
    if (type_ == Type::Bool) return b_;
    if (type_ == Type::Number) return n_ != 0;
    return def;
}

}  // namespace xm

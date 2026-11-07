// json.h - small JSON value type (parse + dump). Objects keep insertion order.
// Written for the camera's replies: UTF-8 text, no comments, no trailing commas.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace xm {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(bool b) : type_(Type::Bool), b_(b) {}
    Json(int n) : type_(Type::Number), n_(n) {}
    Json(double n) : type_(Type::Number), n_(n) {}
    Json(const char* s) : type_(Type::String), s_(s) {}
    Json(std::string s) : type_(Type::String), s_(std::move(s)) {}
    static Json object() { Json j; j.type_ = Type::Object; return j; }
    static Json array() { Json j; j.type_ = Type::Array; return j; }

    // Throws xm::Error(Protocol) on malformed input (also on trailing garbage after the value).
    static Json parse(const std::string& text);
    std::string dump() const;
    static std::string quote(const std::string& s);

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_string() const { return type_ == Type::String; }

    // Object access. A missing key (or a non-object) yields a shared null value, never throws.
    const Json& operator[](const std::string& key) const;
    bool contains(const std::string& key) const;
    Json& set(const std::string& key, Json v);  // object only; replaces an existing key

    // Array access.
    void push(Json v);
    const std::vector<Json>& items() const { return arr_; }

    // Lenient scalar reads: numbers, and strings holding decimal or 0x-hex numbers, both give integers.
    std::string str(const std::string& def = "") const;
    int64_t integer(int64_t def = 0) const;
    bool boolean(bool def = false) const;

private:
    Type type_ = Type::Null;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    std::vector<Json> arr_;
    std::vector<std::pair<std::string, Json>> obj_;
};

}  // namespace xm

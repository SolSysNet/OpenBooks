#pragma once

// A small, strict JSON (RFC 8259) reader and writer for the plugin protocol, manifests and the
// registry. It is deliberately unforgiving, because everything it reads comes from a plugin:
//
//   - Input must be valid UTF-8. Strings may not contain raw control characters or lone
//     surrogate escapes.
//   - Duplicate object keys are an error (two readers could otherwise disagree on the value).
//   - Nesting depth and total size are limited.
//   - Integers outside the 64-bit range are an error rather than silently becoming doubles.
//
// Objects keep their keys in insertion order, so output is deterministic.

#include "openplugin/error.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace opl::json {

class Error : public opl::Error {
public:
    using opl::Error::Error;
};

class Value;
using Array = std::vector<Value>;
using Member = std::pair<std::string, Value>;

// An object: members in insertion order. Keys are unique (set() replaces).
class Object {
public:
    Object() = default;
    Object(std::initializer_list<Member> members);

    const Value* find(std::string_view key) const;
    Value* find(std::string_view key);
    bool contains(std::string_view key) const { return find(key) != nullptr; }
    void set(std::string key, Value value);
    bool erase(std::string_view key);

    std::size_t size() const;
    bool empty() const;
    std::vector<Member>::const_iterator begin() const;
    std::vector<Member>::const_iterator end() const;

    bool operator==(const Object& other) const;
    bool operator!=(const Object& other) const { return !(*this == other); }

private:
    std::vector<Member> members_;
};

enum class Type { Null, Bool, Integer, Double, String, Array, Object };

const char* typeName(Type t);

class Value {
public:
    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : v_(b) {}
    Value(int i) : v_(static_cast<std::int64_t>(i)) {}
    Value(long i) : v_(static_cast<std::int64_t>(i)) {}
    Value(long long i) : v_(static_cast<std::int64_t>(i)) {}
    Value(double d) : v_(d) {}
    Value(const char* s) : v_(std::string(s)) {}
    Value(std::string_view s) : v_(std::string(s)) {}
    Value(std::string s) : v_(std::move(s)) {}
    Value(Array a) : v_(std::move(a)) {}
    Value(Object o) : v_(std::move(o)) {}

    Type type() const { return static_cast<Type>(v_.index()); }
    bool isNull() const { return type() == Type::Null; }
    bool isBool() const { return type() == Type::Bool; }
    bool isInteger() const { return type() == Type::Integer; }
    bool isNumber() const { return type() == Type::Integer || type() == Type::Double; }
    bool isString() const { return type() == Type::String; }
    bool isArray() const { return type() == Type::Array; }
    bool isObject() const { return type() == Type::Object; }

    // Typed access. Each throws json::Error naming the expected and actual type on a mismatch.
    bool asBool() const;
    std::int64_t asInt() const;
    double asDouble() const;  // integers convert
    const std::string& asString() const;
    const Array& asArray() const;
    Array& asArray();
    const Object& asObject() const;
    Object& asObject();

    // Object convenience: nullptr when this isn't an object or has no such key.
    const Value* find(std::string_view key) const;

    bool operator==(const Value& other) const { return v_ == other.v_; }
    bool operator!=(const Value& other) const { return !(*this == other); }

private:
    std::variant<std::monostate, bool, std::int64_t, double, std::string, Array, Object> v_;
};

// Defined here, once Value is complete.
inline std::size_t Object::size() const { return members_.size(); }
inline bool Object::empty() const { return members_.empty(); }
inline std::vector<Member>::const_iterator Object::begin() const { return members_.begin(); }
inline std::vector<Member>::const_iterator Object::end() const { return members_.end(); }

struct Limits {
    std::size_t maxBytes = 16u << 20;  // 16 MiB
    int maxDepth = 64;
};

// Parses exactly one JSON value (surrounding whitespace allowed). Throws json::Error with the
// byte offset of the problem.
Value parse(std::string_view text, const Limits& limits = {});

// Compact output with no newlines, so a value always fits on one protocol line. Throws json::Error
// for NaN/infinite doubles and strings that aren't valid UTF-8.
std::string write(const Value& value);

// Indented output for files people may read (the registry).
std::string writePretty(const Value& value);

// True if `s` is well-formed UTF-8 (no overlong forms, surrogates or code points past U+10FFFF).
bool isValidUtf8(std::string_view s);

}  // namespace opl::json

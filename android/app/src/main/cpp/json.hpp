#pragma once

// Minimal JSON for the Kotlin <-> C++ bridge. Only the subset the bridge needs: objects,
// arrays, strings, integers, booleans and null. Money is always exchanged as decimal
// strings ("1234.50"), never as JSON numbers, so no floating point touches it.

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    long long number = 0;
    std::string string;
    std::vector<Value> array;
    std::vector<std::pair<std::string, Value>> object;

    const Value* find(std::string_view key) const {
        for (const auto& [k, v] : object) {
            if (k == key) return &v;
        }
        return nullptr;
    }
    bool has(std::string_view key) const {
        const Value* v = find(key);
        return v && v->type != Type::Null;
    }
    std::string str(std::string_view key, std::string fallback = {}) const {
        const Value* v = find(key);
        return v && v->type == Type::String ? v->string : fallback;
    }
    long long integer(std::string_view key, long long fallback = 0) const {
        const Value* v = find(key);
        return v && v->type == Type::Number ? v->number : fallback;
    }
    int id(std::string_view key) const { return static_cast<int>(integer(key, 0)); }
    bool flag(std::string_view key, bool fallback = false) const {
        const Value* v = find(key);
        return v && v->type == Type::Bool ? v->boolean : fallback;
    }
    const std::vector<Value>& list(std::string_view key) const {
        static const std::vector<Value> empty;
        const Value* v = find(key);
        return v && v->type == Type::Array ? v->array : empty;
    }
};

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    Value parse() {
        Value v = value(0);
        skip();
        if (pos_ != s_.size()) fail("unexpected trailing data");
        return v;
    }

private:
    [[noreturn]] void fail(const char* what) { throw std::runtime_error(std::string("bad request: ") + what); }
    void skip() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\n' || s_[pos_] == '\r' || s_[pos_] == '\t')) ++pos_;
    }
    char peek() {
        skip();
        if (pos_ >= s_.size()) fail("unexpected end");
        return s_[pos_];
    }
    void expect(char c) {
        if (peek() != c) fail("unexpected character");
        ++pos_;
    }
    bool literal(std::string_view word) {
        if (s_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    Value value(int depth) {
        if (depth > 64) fail("nested too deeply");
        Value v;
        const char c = peek();
        if (c == '{') {
            v.type = Value::Type::Object;
            ++pos_;
            if (peek() == '}') {
                ++pos_;
                return v;
            }
            while (true) {
                if (peek() != '"') fail("expected a key");
                std::string key = string();
                expect(':');
                v.object.emplace_back(std::move(key), value(depth + 1));
                if (peek() == ',') {
                    ++pos_;
                    continue;
                }
                expect('}');
                return v;
            }
        }
        if (c == '[') {
            v.type = Value::Type::Array;
            ++pos_;
            if (peek() == ']') {
                ++pos_;
                return v;
            }
            while (true) {
                v.array.push_back(value(depth + 1));
                if (peek() == ',') {
                    ++pos_;
                    continue;
                }
                expect(']');
                return v;
            }
        }
        if (c == '"') {
            v.type = Value::Type::String;
            v.string = string();
            return v;
        }
        if (literal("true")) {
            v.type = Value::Type::Bool;
            v.boolean = true;
            return v;
        }
        if (literal("false")) {
            v.type = Value::Type::Bool;
            return v;
        }
        if (literal("null")) return v;
        if (c == '-' || (c >= '0' && c <= '9')) {
            v.type = Value::Type::Number;
            bool negative = false;
            if (s_[pos_] == '-') {
                negative = true;
                ++pos_;
            }
            long long n = 0;
            bool any = false;
            while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
                if (n > 100000000000000LL) fail("number too large");
                n = n * 10 + (s_[pos_++] - '0');
                any = true;
            }
            if (!any) fail("bad number");
            if (pos_ < s_.size() && (s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E'))
                fail("only integers are accepted; send amounts as strings");
            v.number = negative ? -n : n;
            return v;
        }
        fail("unexpected character");
    }

    static void appendUtf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    std::uint32_t hex4() {
        if (pos_ + 4 > s_.size()) fail("bad escape");
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = s_[pos_++];
            v <<= 4;
            if (h >= '0' && h <= '9') v |= static_cast<std::uint32_t>(h - '0');
            else if (h >= 'a' && h <= 'f') v |= static_cast<std::uint32_t>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') v |= static_cast<std::uint32_t>(h - 'A' + 10);
            else fail("bad escape");
        }
        return v;
    }

    std::string string() {
        expect('"');
        std::string out;
        while (true) {
            if (pos_ >= s_.size()) fail("unterminated string");
            const char c = s_[pos_++];
            if (c == '"') return out;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) fail("bad escape");
            const char e = s_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    std::uint32_t cp = hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF && s_.substr(pos_, 2) == "\\u") {  // surrogate pair
                        pos_ += 2;
                        const std::uint32_t low = hex4();
                        if (low < 0xDC00 || low > 0xDFFF) fail("bad surrogate pair");
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: fail("bad escape");
            }
        }
    }

    std::string_view s_;
    std::size_t pos_ = 0;
};

inline Value parse(std::string_view text) { return Parser(text).parse(); }

// Streaming writer: commas are handled automatically.
class Writer {
public:
    Writer& beginObject() { return open('{'); }
    Writer& endObject() { return close('}'); }
    Writer& beginArray() { return open('['); }
    Writer& endArray() { return close(']'); }
    Writer& key(std::string_view k) {
        separator();
        quote(k);
        out_ += ':';
        afterKey_ = true;
        return *this;
    }
    Writer& value(std::string_view v) {
        separator();
        quote(v);
        return *this;
    }
    Writer& value(const char* v) { return value(std::string_view(v)); }
    Writer& value(const std::string& v) { return value(std::string_view(v)); }
    Writer& value(long long v) {
        separator();
        out_ += std::to_string(v);
        return *this;
    }
    Writer& value(int v) { return value(static_cast<long long>(v)); }
    Writer& value(unsigned v) { return value(static_cast<long long>(v)); }
    Writer& value(bool v) {
        separator();
        out_ += v ? "true" : "false";
        return *this;
    }
    Writer& null() {
        separator();
        out_ += "null";
        return *this;
    }
    template <typename T>
    Writer& field(std::string_view k, const T& v) {
        key(k);
        return value(v);
    }
    const std::string& str() const { return out_; }

private:
    Writer& open(char c) {
        separator();
        out_ += c;
        first_.push_back(true);
        return *this;
    }
    Writer& close(char c) {
        out_ += c;
        first_.pop_back();
        return *this;
    }
    void separator() {
        if (afterKey_) {
            afterKey_ = false;
            return;
        }
        if (!first_.empty()) {
            if (!first_.back()) out_ += ',';
            first_.back() = false;
        }
    }
    void quote(std::string_view s) {
        out_ += '"';
        for (char ch : s) {
            const auto c = static_cast<unsigned char>(ch);
            switch (ch) {
                case '"': out_ += "\\\""; break;
                case '\\': out_ += "\\\\"; break;
                case '\n': out_ += "\\n"; break;
                case '\r': out_ += "\\r"; break;
                case '\t': out_ += "\\t"; break;
                default:
                    if (c < 0x20) {
                        static const char* hex = "0123456789abcdef";
                        out_ += "\\u00";
                        out_ += hex[c >> 4];
                        out_ += hex[c & 15];
                    } else {
                        out_ += ch;  // UTF-8 passes through unchanged
                    }
            }
        }
        out_ += '"';
    }

    std::string out_;
    std::vector<bool> first_;
    bool afterKey_ = false;
};

}  // namespace json

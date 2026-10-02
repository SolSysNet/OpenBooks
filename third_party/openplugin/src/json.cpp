#include "openplugin/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace opl::json {

// ---------------------------------------------------------------------- Object

Object::Object(std::initializer_list<Member> members) {
    for (const Member& m : members) set(m.first, m.second);
}

const Value* Object::find(std::string_view key) const {
    for (const Member& m : members_)
        if (m.first == key) return &m.second;
    return nullptr;
}

Value* Object::find(std::string_view key) {
    for (Member& m : members_)
        if (m.first == key) return &m.second;
    return nullptr;
}

void Object::set(std::string key, Value value) {
    if (Value* existing = find(key)) {
        *existing = std::move(value);
        return;
    }
    members_.emplace_back(std::move(key), std::move(value));
}

bool Object::erase(std::string_view key) {
    for (auto it = members_.begin(); it != members_.end(); ++it) {
        if (it->first == key) {
            members_.erase(it);
            return true;
        }
    }
    return false;
}

// Order-insensitive: two objects are equal when they have the same keys with equal values.
bool Object::operator==(const Object& other) const {
    if (members_.size() != other.members_.size()) return false;
    for (const Member& m : members_) {
        const Value* v = other.find(m.first);
        if (!v || !(*v == m.second)) return false;
    }
    return true;
}

// ----------------------------------------------------------------------- Value

const char* typeName(Type t) {
    switch (t) {
        case Type::Null: return "null";
        case Type::Bool: return "boolean";
        case Type::Integer: return "integer";
        case Type::Double: return "number";
        case Type::String: return "string";
        case Type::Array: return "array";
        case Type::Object: return "object";
    }
    return "?";
}

namespace {

[[noreturn]] void typeMismatch(Type expected, Type actual) {
    throw Error(std::string("expected ") + typeName(expected) + ", found " + typeName(actual));
}

}  // namespace

bool Value::asBool() const {
    if (!isBool()) typeMismatch(Type::Bool, type());
    return std::get<bool>(v_);
}

std::int64_t Value::asInt() const {
    if (!isInteger()) typeMismatch(Type::Integer, type());
    return std::get<std::int64_t>(v_);
}

double Value::asDouble() const {
    if (isInteger()) return static_cast<double>(std::get<std::int64_t>(v_));
    if (type() != Type::Double) typeMismatch(Type::Double, type());
    return std::get<double>(v_);
}

const std::string& Value::asString() const {
    if (!isString()) typeMismatch(Type::String, type());
    return std::get<std::string>(v_);
}

const Array& Value::asArray() const {
    if (!isArray()) typeMismatch(Type::Array, type());
    return std::get<Array>(v_);
}

Array& Value::asArray() {
    if (!isArray()) typeMismatch(Type::Array, type());
    return std::get<Array>(v_);
}

const Object& Value::asObject() const {
    if (!isObject()) typeMismatch(Type::Object, type());
    return std::get<Object>(v_);
}

Object& Value::asObject() {
    if (!isObject()) typeMismatch(Type::Object, type());
    return std::get<Object>(v_);
}

const Value* Value::find(std::string_view key) const {
    if (!isObject()) return nullptr;
    return std::get<Object>(v_).find(key);
}

// ------------------------------------------------------------------------ UTF-8

namespace {

// Decodes one UTF-8 sequence at s[i]. Returns its length, or 0 if it's malformed.
std::size_t utf8Sequence(std::string_view s, std::size_t i, std::uint32_t* cp) {
    const auto b = [&](std::size_t k) { return static_cast<unsigned char>(s[i + k]); };
    const unsigned char c = b(0);
    std::size_t len;
    std::uint32_t value;
    if (c < 0x80) {
        *cp = c;
        return 1;
    } else if ((c & 0xE0) == 0xC0) {
        len = 2;
        value = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
        value = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4;
        value = c & 0x07;
    } else {
        return 0;
    }
    if (i + len > s.size()) return 0;
    for (std::size_t k = 1; k < len; ++k) {
        if ((b(k) & 0xC0) != 0x80) return 0;
        value = (value << 6) | (b(k) & 0x3F);
    }
    static const std::uint32_t minimum[] = {0, 0, 0x80, 0x800, 0x10000};
    if (value < minimum[len]) return 0;                    // overlong
    if (value >= 0xD800 && value <= 0xDFFF) return 0;      // surrogate
    if (value > 0x10FFFF) return 0;
    *cp = value;
    return len;
}

void appendUtf8(std::string& out, std::uint32_t cp) {
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

}  // namespace

bool isValidUtf8(std::string_view s) {
    std::uint32_t cp;
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t n = utf8Sequence(s, i, &cp);
        if (n == 0) return false;
        i += n;
    }
    return true;
}

// ----------------------------------------------------------------------- parser

namespace {

class Parser {
public:
    Parser(std::string_view text, const Limits& limits) : s_(text), limits_(limits) {}

    Value parseDocument() {
        if (s_.size() > limits_.maxBytes)
            throw Error("JSON is larger than the " + std::to_string(limits_.maxBytes) + "-byte limit");
        skipWhitespace();
        Value v = parseValue(0);
        skipWhitespace();
        if (pos_ != s_.size()) fail("unexpected data after the JSON value");
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& message) const {
        throw Error("invalid JSON at byte " + std::to_string(pos_) + ": " + message);
    }

    bool atEnd() const { return pos_ >= s_.size(); }
    char peek() const { return atEnd() ? '\0' : s_[pos_]; }

    void skipWhitespace() {
        while (!atEnd()) {
            const char c = s_[pos_];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
            ++pos_;
        }
    }

    void expectLiteral(std::string_view word) {
        if (s_.substr(pos_, word.size()) != word) fail("unexpected character");
        pos_ += word.size();
    }

    Value parseValue(int depth) {
        if (atEnd()) fail("unexpected end of input");
        switch (peek()) {
            case '{': return parseObject(depth + 1);
            case '[': return parseArray(depth + 1);
            case '"': return Value(parseString());
            case 't': expectLiteral("true"); return Value(true);
            case 'f': expectLiteral("false"); return Value(false);
            case 'n': expectLiteral("null"); return Value();
            default:
                if (peek() == '-' || (peek() >= '0' && peek() <= '9')) return parseNumber();
                fail("unexpected character");
        }
    }

    void checkDepth(int depth) const {
        if (depth > limits_.maxDepth)
            throw Error("JSON is nested more than " + std::to_string(limits_.maxDepth) + " levels deep");
    }

    Value parseObject(int depth) {
        checkDepth(depth);
        ++pos_;  // {
        Object obj;
        skipWhitespace();
        if (peek() == '}') {
            ++pos_;
            return Value(std::move(obj));
        }
        for (;;) {
            skipWhitespace();
            if (peek() != '"') fail("expected a string key");
            const std::size_t keyPos = pos_;
            std::string key = parseString();
            if (obj.contains(key)) {
                pos_ = keyPos;
                fail("duplicate key \"" + key + "\"");
            }
            skipWhitespace();
            if (peek() != ':') fail("expected ':'");
            ++pos_;
            skipWhitespace();
            Value v = parseValue(depth);
            obj.set(std::move(key), std::move(v));
            skipWhitespace();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                return Value(std::move(obj));
            }
            fail("expected ',' or '}'");
        }
    }

    Value parseArray(int depth) {
        checkDepth(depth);
        ++pos_;  // [
        Array arr;
        skipWhitespace();
        if (peek() == ']') {
            ++pos_;
            return Value(std::move(arr));
        }
        for (;;) {
            skipWhitespace();
            arr.push_back(parseValue(depth));
            skipWhitespace();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                return Value(std::move(arr));
            }
            fail("expected ',' or ']'");
        }
    }

    unsigned parseHex4() {
        if (pos_ + 4 > s_.size()) fail("truncated \\u escape");
        unsigned v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else fail("invalid \\u escape");
        }
        return v;
    }

    std::string parseString() {
        ++pos_;  // opening quote
        std::string out;
        for (;;) {
            if (atEnd()) fail("unterminated string");
            const unsigned char c = static_cast<unsigned char>(s_[pos_]);
            if (c == '"') {
                ++pos_;
                return out;
            }
            if (c < 0x20) fail("control character in string");
            if (c == '\\') {
                ++pos_;
                if (atEnd()) fail("unterminated string");
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
                        std::uint32_t cp = parseHex4();
                        if (cp >= 0xDC00 && cp <= 0xDFFF) fail("lone low surrogate in \\u escape");
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (s_.substr(pos_, 2) != "\\u") fail("lone high surrogate in \\u escape");
                            pos_ += 2;
                            const std::uint32_t low = parseHex4();
                            if (low < 0xDC00 || low > 0xDFFF) fail("invalid surrogate pair in \\u escape");
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        }
                        appendUtf8(out, cp);
                        break;
                    }
                    default: fail("invalid escape");
                }
                continue;
            }
            std::uint32_t cp;
            const std::size_t n = utf8Sequence(s_, pos_, &cp);
            if (n == 0) fail("invalid UTF-8");
            out.append(s_.data() + pos_, n);
            pos_ += n;
        }
    }

    Value parseNumber() {
        const std::size_t start = pos_;
        bool integral = true;
        if (peek() == '-') ++pos_;
        if (peek() == '0') {
            ++pos_;
        } else if (peek() >= '1' && peek() <= '9') {
            while (peek() >= '0' && peek() <= '9') ++pos_;
        } else {
            fail("invalid number");
        }
        if (peek() == '.') {
            integral = false;
            ++pos_;
            if (!(peek() >= '0' && peek() <= '9')) fail("invalid number");
            while (peek() >= '0' && peek() <= '9') ++pos_;
        }
        if (peek() == 'e' || peek() == 'E') {
            integral = false;
            ++pos_;
            if (peek() == '+' || peek() == '-') ++pos_;
            if (!(peek() >= '0' && peek() <= '9')) fail("invalid number");
            while (peek() >= '0' && peek() <= '9') ++pos_;
        }
        const std::string text(s_.substr(start, pos_ - start));
        if (integral) {
            // Accumulate exactly; anything outside int64 is an error, never a silent double.
            const bool negative = text[0] == '-';
            std::uint64_t magnitude = 0;
            const std::uint64_t limit = negative ? std::uint64_t(1) << 63 : (std::uint64_t(1) << 63) - 1;
            for (std::size_t k = negative ? 1 : 0; k < text.size(); ++k) {
                const unsigned digit = static_cast<unsigned>(text[k] - '0');
                if (magnitude > (limit - digit) / 10) {
                    pos_ = start;
                    fail("integer out of range");
                }
                magnitude = magnitude * 10 + digit;
            }
            if (negative) {
                if (magnitude == std::uint64_t(1) << 63) return Value(std::numeric_limits<std::int64_t>::min());
                return Value(-static_cast<std::int64_t>(magnitude));
            }
            return Value(static_cast<std::int64_t>(magnitude));
        }
        // strtod is locale-sensitive; the protocol never changes LC_NUMERIC from "C".
        const double d = std::strtod(text.c_str(), nullptr);
        if (!std::isfinite(d)) {
            pos_ = start;
            fail("number out of range");
        }
        return Value(d);
    }

    std::string_view s_;
    Limits limits_;
    std::size_t pos_ = 0;
};

}  // namespace

Value parse(std::string_view text, const Limits& limits) {
    return Parser(text, limits).parseDocument();
}

// ----------------------------------------------------------------------- writer

namespace {

void writeString(std::string& out, const std::string& s) {
    if (!isValidUtf8(s)) throw Error("cannot write a string that isn't valid UTF-8");
    out += '"';
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += ch;
                }
        }
    }
    out += '"';
}

void writeValue(std::string& out, const Value& v, int indent, int level) {
    const auto newline = [&](int lvl) {
        if (indent <= 0) return;
        out += '\n';
        out.append(static_cast<std::size_t>(indent * lvl), ' ');
    };
    switch (v.type()) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += v.asBool() ? "true" : "false"; break;
        case Type::Integer: out += std::to_string(v.asInt()); break;
        case Type::Double: {
            const double d = v.asDouble();
            if (!std::isfinite(d)) throw Error("cannot write a NaN or infinite number");
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.17g", d);
            std::string text = buf;
            // Keep it a double on the way back in.
            if (text.find_first_of(".eEn") == std::string::npos) text += ".0";
            out += text;
            break;
        }
        case Type::String: writeString(out, v.asString()); break;
        case Type::Array: {
            const Array& a = v.asArray();
            out += '[';
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (i) out += ',';
                newline(level + 1);
                writeValue(out, a[i], indent, level + 1);
            }
            if (!a.empty()) newline(level);
            out += ']';
            break;
        }
        case Type::Object: {
            const Object& o = v.asObject();
            out += '{';
            bool first = true;
            for (const Member& m : o) {
                if (!first) out += ',';
                first = false;
                newline(level + 1);
                writeString(out, m.first);
                out += indent > 0 ? ": " : ":";
                writeValue(out, m.second, indent, level + 1);
            }
            if (!o.empty()) newline(level);
            out += '}';
            break;
        }
    }
}

}  // namespace

std::string write(const Value& value) {
    std::string out;
    writeValue(out, value, 0, 0);
    return out;
}

std::string writePretty(const Value& value) {
    std::string out;
    writeValue(out, value, 2, 0);
    out += '\n';
    return out;
}

}  // namespace opl::json

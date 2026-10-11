// mcpp — a zero-dependency C++17 SDK for the Model Context Protocol.
//
// json.hpp: self-contained JSON value, parser and serializer.
// Design goals: exact int64 round-trip, deterministic output (sorted keys),
// precise error offsets, no exceptions used for control flow on hot paths
// (parse throws mcpp::Json::Error only on malformed input).

#pragma once

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace mcpp {

class Json {
public:
    enum class Type { Null, Bool, Int, Double, String, Array, Object };

    struct Error : std::runtime_error {
        size_t offset;
        explicit Error(const std::string& msg, size_t off)
            : std::runtime_error(msg + " at offset " + std::to_string(off)), offset(off) {}
    };

    // ------------------------------------------------------------------ ctor

    Json() : type_(Type::Null) {}
    Json(std::nullptr_t) : type_(Type::Null) {}
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    Json(int v) : type_(Type::Int), int_(v) {}
    Json(long v) : type_(Type::Int), int_(static_cast<int64_t>(v)) {}
    Json(long long v) : type_(Type::Int), int_(static_cast<int64_t>(v)) {}
    Json(unsigned v) : type_(Type::Int), int_(static_cast<int64_t>(v)) {}
    Json(double v) : type_(Type::Double), dbl_(v) {}
    Json(const char* v) : type_(Type::String), str_(v ? v : "") {}
    Json(std::string v) : type_(Type::String), str_(std::move(v)) {}

    // Brace-friendly object construction: Json{{"name", "x"}, {"n", 1}}.
    Json(std::initializer_list<std::pair<const std::string, Json>> init)
        : type_(Type::Object) {
        for (const auto& kv : init) obj_.emplace(kv.first, kv.second);
    }

    static Json object() { Json j; j.type_ = Type::Object; return j; }
    static Json object(std::initializer_list<std::pair<const std::string, Json>> init) {
        return Json(init);
    }
    static Json array() { Json j; j.type_ = Type::Array; return j; }

    // --------------------------------------------------------------- inspect

    Type type() const { return type_; }
    const char* type_name() const {
        switch (type_) {
            case Type::Null: return "null";
            case Type::Bool: return "bool";
            case Type::Int: return "int";
            case Type::Double: return "double";
            case Type::String: return "string";
            case Type::Array: return "array";
            case Type::Object: return "object";
        }
        return "?";
    }

    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_int() const { return type_ == Type::Int; }
    bool is_double() const { return type_ == Type::Double; }
    bool is_number() const { return is_int() || is_double(); }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool as_bool() const {
        check(Type::Bool, "bool");
        return bool_;
    }
    int64_t as_int() const {
        if (type_ == Type::Int) return int_;
        if (type_ == Type::Double) {
            if (std::fmod(dbl_, 1.0) != 0.0) throw_wrong("int");
            return static_cast<int64_t>(dbl_);
        }
        throw_wrong("int");
    }
    double as_double() const {
        if (type_ == Type::Double) return dbl_;
        if (type_ == Type::Int) return static_cast<double>(int_);
        throw_wrong("double");
    }
    const std::string& as_string() const {
        check(Type::String, "string");
        return str_;
    }
    const std::vector<Json>& as_array() const {
        check(Type::Array, "array");
        return arr_;
    }
    const std::map<std::string, Json>& as_object() const {
        check(Type::Object, "object");
        return obj_;
    }

    size_t size() const {
        if (type_ == Type::Array) return arr_.size();
        if (type_ == Type::Object) return obj_.size();
        if (type_ == Type::Null) return 0;
        throw_wrong("array or object");
    }

    // -------------------------------------------------------- object access

    bool contains(const std::string& key) const {
        return type_ == Type::Object && obj_.find(key) != obj_.end();
    }

    // Read access; returns nullptr when the key is absent (or not an object).
    const Json* find(const std::string& key) const {
        if (type_ != Type::Object) return nullptr;
        auto it = obj_.find(key);
        return it == obj_.end() ? nullptr : &it->second;
    }

    // Throwing read access with a precise message — used on hot protocol paths.
    const Json& get(const std::string& key) const {
        const Json* p = find(key);
        if (!p) {
            if (type_ != Type::Object)
                throw std::logic_error("mcpp::Json: expected object, got " + std::string(type_name()));
            throw std::logic_error("mcpp::Json: missing key \"" + key + "\"");
        }
        return *p;
    }

    template <typename T>
    T get_or(const std::string& key, T def) const {
        const Json* p = find(key);
        if (!p) return def;
        try {
            if constexpr (std::is_same_v<T, std::string>) return p->as_string();
            else if constexpr (std::is_same_v<T, bool>) return p->as_bool();
            else if constexpr (std::is_same_v<T, double>) return p->as_double();
            else return static_cast<T>(p->as_int());
        } catch (...) {
            return def;
        }
    }

    // Write access, nlohmann-style: promotes Null to Object, throws if the
    // value is another type. Returns a reference to the (created) member.
    Json& operator[](const std::string& key) {
        if (type_ == Type::Null) type_ = Type::Object;
        if (type_ != Type::Object)
            throw std::logic_error("mcpp::Json: operator[] on non-object " + std::string(type_name()));
        return obj_[key];
    }

    // Read-only index on const objects; throws when the key is absent.
    const Json& operator[](const std::string& key) const { return get(key); }

    void set(const std::string& key, Json v) { (*this)[key] = std::move(v); }
    void erase(const std::string& key) {
        if (type_ == Type::Object) obj_.erase(key);
    }

    // --------------------------------------------------------- array access

    // Appends to an array (promotes Null); returns *this for chaining:
    //   arr.push_back(1).push_back(2);
    Json& push_back(Json v) {
        if (type_ == Type::Null) type_ = Type::Array;
        if (type_ != Type::Array)
            throw std::logic_error("mcpp::Json: push_back on non-array " + std::string(type_name()));
        arr_.push_back(std::move(v));
        return *this;
    }

    const Json& at(size_t i) const {
        check(Type::Array, "array");
        if (i >= arr_.size())
            throw std::out_of_range("mcpp::Json: index " + std::to_string(i) + " >= " + std::to_string(arr_.size()));
        return arr_[i];
    }

    // ------------------------------------------------------------ serialize

    std::string dump(int indent = -1) const {
        std::string out;
        out.reserve(64);
        write(out, indent, 0);
        return out;
    }

    // --------------------------------------------------------------- parse

    static Json parse(std::string_view text) { return Parser(text).run(); }

    // ----------------------------------------------------------- equality

    bool operator==(const Json& o) const { return equals(*this, o); }
    bool operator!=(const Json& o) const { return !equals(*this, o); }

private:
    void check(Type t, const char* want) const {
        if (type_ != t)
            throw std::logic_error(std::string("mcpp::Json: expected ") + want + ", got " + type_name());
    }
    [[noreturn]] int64_t throw_wrong(const char* want) const {
        throw std::logic_error(std::string("mcpp::Json: expected ") + want + ", got " + type_name());
    }

    static bool equals(const Json& a, const Json& b) {
        if (a.is_number() && b.is_number())
            return a.is_int() && b.is_int() ? a.int_ == b.int_
                                            : a.as_double() == b.as_double();
        if (a.type_ != b.type_) return false;
        switch (a.type_) {
            case Type::Null: return true;
            case Type::Bool: return a.bool_ == b.bool_;
            case Type::String: return a.str_ == b.str_;
            case Type::Array: return a.arr_ == b.arr_;
            case Type::Object: return a.obj_ == b.obj_;
            default: return false;
        }
    }

    void write(std::string& out, int indent, int level) const {
        auto newline = [&](int lvl) {
            if (indent < 0) return;
            out.push_back('\n');
            out.append(static_cast<size_t>(indent) * lvl, ' ');
        };
        switch (type_) {
            case Type::Null: out += "null"; break;
            case Type::Bool: out += bool_ ? "true" : "false"; break;
            case Type::Int: out += std::to_string(int_); break;
            case Type::Double: out += format_double(dbl_); break;
            case Type::String: write_string(out, str_); break;
            case Type::Array: {
                if (arr_.empty()) { out += "[]"; break; }
                out.push_back('[');
                bool first = true;
                for (const Json& v : arr_) {
                    if (!first) out.push_back(',');
                    first = false;
                    newline(level + 1);
                    v.write(out, indent, level + 1);
                }
                newline(level);
                out.push_back(']');
                break;
            }
            case Type::Object: {
                if (obj_.empty()) { out += "{}"; break; }
                out.push_back('{');
                bool first = true;
                for (const auto& [k, v] : obj_) {
                    if (!first) out.push_back(',');
                    first = false;
                    newline(level + 1);
                    write_string(out, k);
                    out.push_back(':');
                    if (indent >= 0) out.push_back(' ');
                    v.write(out, indent, level + 1);
                }
                newline(level);
                out.push_back('}');
                break;
            }
        }
    }

    static void write_string(std::string& out, const std::string& s) {
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
                        std::snprintf(buf, sizeof buf, "\\u%04x", c);
                        out += buf;
                    } else {
                        out.push_back(static_cast<char>(c));  // UTF-8 passthrough
                    }
            }
        }
        out.push_back('"');
    }

    // Shortest representation that round-trips through strtod.
    static std::string format_double(double d) {
        if (!std::isfinite(d)) return "null";  // JSON has no NaN/Inf
        char buf[64];
        for (int prec : {15, 16, 17}) {
            std::snprintf(buf, sizeof buf, "%.*g", prec, d);
            if (std::strtod(buf, nullptr) == d) break;
        }
        return buf;
    }

    // ---------------------------------------------------------------- parser

    class Parser {
    public:
        explicit Parser(std::string_view text) : text_(text) {}

        Json run() {
            Json v = parse_value(0);
            skip_ws();
            if (pos_ != text_.size())
                fail("unexpected trailing content");
            return v;
        }

    private:
        std::string_view text_;
        size_t pos_ = 0;
        static constexpr int kMaxDepth = 200;

        [[noreturn]] void fail(const std::string& msg) const {
            throw Json::Error("mcpp::Json: " + msg, pos_);
        }

        void skip_ws() {
            while (pos_ < text_.size()) {
                char c = text_[pos_];
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
                else break;
            }
        }

        char peek() {
            if (pos_ >= text_.size()) fail("unexpected end of input");
            return text_[pos_];
        }

        void expect(char c) {
            if (pos_ >= text_.size() || text_[pos_] != c)
                fail(std::string("expected '") + c + "'");
            ++pos_;
        }

        bool literal(std::string_view word) {
            if (text_.compare(pos_, word.size(), word) == 0) {
                pos_ += word.size();
                return true;
            }
            return false;
        }

        Json parse_value(int depth) {
            if (depth > kMaxDepth) fail("nesting too deep");
            skip_ws();
            char c = peek();
            switch (c) {
                case '{': return parse_object(depth);
                case '[': return parse_array(depth);
                case '"': return Json(parse_string());
                case 't': if (literal("true")) return Json(true); fail("invalid literal");
                case 'f': if (literal("false")) return Json(false); fail("invalid literal");
                case 'n': if (literal("null")) return Json(nullptr); fail("invalid literal");
                default: return parse_number();
            }
        }

        Json parse_object(int depth) {
            expect('{');
            Json out = Json::object();
            skip_ws();
            if (peek() == '}') { ++pos_; return out; }
            for (;;) {
                skip_ws();
                if (peek() != '"') fail("expected object key string");
                std::string key = parse_string();
                skip_ws();
                expect(':');
                out.obj_.emplace(std::move(key), parse_value(depth + 1));
                skip_ws();
                char c = peek();
                if (c == ',') { ++pos_; continue; }
                if (c == '}') { ++pos_; return out; }
                fail("expected ',' or '}' in object");
            }
        }

        Json parse_array(int depth) {
            expect('[');
            Json out = Json::array();
            skip_ws();
            if (peek() == ']') { ++pos_; return out; }
            for (;;) {
                out.arr_.push_back(parse_value(depth + 1));
                skip_ws();
                char c = peek();
                if (c == ',') { ++pos_; continue; }
                if (c == ']') { ++pos_; return out; }
                fail("expected ',' or ']' in array");
            }
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

        uint32_t parse_hex4() {
            if (text_.size() - pos_ < 4) fail("truncated \\u escape");
            uint32_t v = 0;
            for (int i = 0; i < 4; ++i) {
                char c = text_[pos_++];
                v <<= 4;
                if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
                else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
                else fail("invalid \\u escape");
            }
            return v;
        }

        std::string parse_string() {
            expect('"');
            std::string out;
            for (;;) {
                if (pos_ >= text_.size()) fail("unterminated string");
                unsigned char c = static_cast<unsigned char>(text_[pos_]);
                if (c == '"') { ++pos_; return out; }
                if (c < 0x20) fail("unescaped control character in string");
                if (c == '\\') {
                    ++pos_;
                    if (pos_ >= text_.size()) fail("unterminated escape");
                    char e = text_[pos_++];
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
                            uint32_t cp = parse_hex4();
                            if (cp >= 0xD800 && cp <= 0xDBFF) {  // high surrogate
                                if (pos_ + 1 >= text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u')
                                    fail("lone high surrogate");
                                pos_ += 2;
                                uint32_t lo = parse_hex4();
                                if (lo < 0xDC00 || lo > 0xDFFF) fail("invalid low surrogate");
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                                fail("lone low surrogate");
                            }
                            append_utf8(out, cp);
                            break;
                        }
                        default: fail("invalid escape character");
                    }
                } else {
                    out.push_back(static_cast<char>(c));
                    ++pos_;
                }
            }
        }

        Json parse_number() {
            char first = peek();
            if (!(first == '-' || (first >= '0' && first <= '9')))
                fail("value must start with '-' or a digit");
            size_t start = pos_;
            bool is_int = true;
            if (peek() == '-') ++pos_;
            while (pos_ < text_.size()) {
                char c = text_[pos_];
                if (c >= '0' && c <= '9') { ++pos_; continue; }
                if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                    // '+'/'-' only valid right after e/E; strtod will catch misuse.
                    if (c == '+' || c == '-') {
                        char prev = text_[pos_ - 1];
                        if (prev != 'e' && prev != 'E') fail("invalid character in number");
                    }
                    is_int = false;
                    ++pos_;
                    continue;
                }
                break;
            }
            size_t len = pos_ - start;
            if (len == 0 || (len == 1 && text_[start] == '-')) fail("invalid number");
            if (len >= 64) fail("number too long");
            char buf[64];
            std::memcpy(buf, text_.data() + start, len);
            buf[len] = '\0';
            char* end = nullptr;
            if (is_int) {
                errno = 0;
                long long v = std::strtoll(buf, &end, 10);
                if (errno == ERANGE) { is_int = false; }  // fall through to double
                else return Json(static_cast<int64_t>(v));
            }
            errno = 0;
            double d = std::strtod(buf, &end);
            if (end != buf + len) fail("invalid number");
            return Json(d);
        }
    };

    Type type_;
    bool bool_ = false;
    int64_t int_ = 0;
    double dbl_ = 0.0;
    std::string str_;
    std::vector<Json> arr_;
    std::map<std::string, Json> obj_;
};

}  // namespace mcpp

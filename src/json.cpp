// SPDX-License-Identifier: AGPL-3.0-or-later
#include "json.h"

#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>

namespace hikari {
namespace {

class Parser {
public:
    Parser(std::string_view input, std::size_t maxDepth) : input_(input), maxDepth_(maxDepth) {}
    Json run() {
        if (!isValidUtf8(input_)) fail("Invalid UTF-8");
        auto result = value(0);
        whitespace();
        if (position_ != input_.size()) fail("Trailing JSON data");
        return result;
    }

private:
    [[noreturn]] void fail(const char* message) const { throw JsonError(message); }
    void whitespace() {
        while (position_ < input_.size()) {
            const char ch = input_[position_];
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') break;
            ++position_;
        }
    }
    bool take(char ch) {
        if (position_ < input_.size() && input_[position_] == ch) { ++position_; return true; }
        return false;
    }
    void expect(char ch) { if (!take(ch)) fail("Unexpected JSON token"); }
    unsigned hex4() {
        unsigned result = 0;
        for (int i = 0; i < 4; ++i) {
            if (position_ == input_.size()) fail("Incomplete Unicode escape");
            const char ch = input_[position_++];
            unsigned digit = 0;
            if (ch >= '0' && ch <= '9') digit = ch - '0';
            else if (ch >= 'a' && ch <= 'f') digit = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') digit = ch - 'A' + 10;
            else fail("Invalid Unicode escape");
            result = result * 16 + digit;
        }
        return result;
    }
    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp <= 0x7f) out.push_back(static_cast<char>(cp));
        else if (cp <= 0x7ff) {
            out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 63)));
        } else if (cp <= 0xffff) {
            out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
            out.push_back(static_cast<char>(0x80 | (cp & 63)));
        } else {
            out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 63)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 63)));
            out.push_back(static_cast<char>(0x80 | (cp & 63)));
        }
    }
    std::string string() {
        expect('"');
        std::string result;
        while (position_ < input_.size()) {
            const auto ch = static_cast<unsigned char>(input_[position_++]);
            if (ch == '"') return result;
            if (ch < 0x20) fail("Unescaped control character");
            if (ch != '\\') { result.push_back(static_cast<char>(ch)); continue; }
            if (position_ == input_.size()) fail("Incomplete string escape");
            switch (input_[position_++]) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                unsigned cp = hex4();
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    expect('\\'); expect('u');
                    const unsigned low = hex4();
                    if (low < 0xdc00 || low > 0xdfff) fail("Invalid surrogate pair");
                    cp = 0x10000 + (cp - 0xd800) * 1024 + low - 0xdc00;
                } else if (cp >= 0xdc00 && cp <= 0xdfff) fail("Unpaired low surrogate");
                appendUtf8(result, cp);
                break;
            }
            default: fail("Invalid string escape");
            }
        }
        fail("Unterminated JSON string");
    }
    bool digit() const { return position_ < input_.size() && input_[position_] >= '0' && input_[position_] <= '9'; }
    Json number() {
        const auto begin = position_;
        take('-');
        if (take('0')) { if (digit()) fail("Leading zero"); }
        else { if (!digit()) fail("Invalid number"); while (digit()) ++position_; }
        if (take('.')) { if (!digit()) fail("Missing decimal digits"); while (digit()) ++position_; }
        if (take('e') || take('E')) {
            if (!take('+')) take('-');
            if (!digit()) fail("Missing exponent digits");
            while (digit()) ++position_;
        }
        double result = 0;
        const auto converted = std::from_chars(input_.data() + begin, input_.data() + position_, result);
        if (converted.ec != std::errc{} || converted.ptr != input_.data() + position_ || !std::isfinite(result))
            fail("Number outside finite range");
        return result;
    }
    Json literal(std::string_view text, Json result) {
        if (input_.substr(position_, text.size()) != text) fail("Invalid literal");
        position_ += text.size();
        return result;
    }
    Json value(std::size_t depth) {
        whitespace();
        if (position_ == input_.size()) fail("Missing JSON value");
        const char ch = input_[position_];
        if (ch == '"') return string();
        if (ch == 't') return literal("true", true);
        if (ch == 'f') return literal("false", false);
        if (ch == 'n') return literal("null", nullptr);
        if (ch == '-' || (ch >= '0' && ch <= '9')) return number();
        if (ch != '[' && ch != '{') fail("Invalid JSON value");
        if (depth >= maxDepth_) fail("JSON depth limit");
        ++position_;
        whitespace();
        if (ch == '[') {
            Json::Array result;
            if (take(']')) return result;
            do { result.push_back(value(depth + 1)); whitespace(); if (take(']')) return result; expect(','); } while (true);
        }
        Json::Object result;
        if (take('}')) return result;
        do {
            whitespace();
            auto key = string();
            whitespace(); expect(':');
            auto member = value(depth + 1);
            if (!result.emplace(std::move(key), std::move(member)).second) fail("Duplicate object key");
            whitespace();
            if (take('}')) return result;
            expect(',');
        } while (true);
    }
    std::string_view input_;
    std::size_t maxDepth_;
    std::size_t position_ = 0;
};

void quote(std::string& out, const std::string& value) {
    if (!isValidUtf8(value)) throw JsonError("Invalid UTF-8 string");
    static constexpr char hex[] = "0123456789abcdef";
    out.push_back('"');
    for (const unsigned char ch : value) {
        if (ch == '"' || ch == '\\') { out.push_back('\\'); out.push_back(static_cast<char>(ch)); }
        else if (ch < 0x20) {
            out += "\\u00"; out.push_back(hex[ch >> 4]); out.push_back(hex[ch & 15]);
        } else out.push_back(static_cast<char>(ch));
    }
    out.push_back('"');
}

void serialize(std::string& out, const Json& value, std::size_t depth) {
    if (depth > 64) throw JsonError("JSON depth limit");
    if (value.isNull()) out += "null";
    else if (value.isBool()) out += value.asBool() ? "true" : "false";
    else if (value.isNumber()) {
        const auto number = value.asNumber();
        if (!std::isfinite(number)) throw JsonError("Number outside finite range");
        char buffer[64];
        const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), number, std::chars_format::general,
                                              std::numeric_limits<double>::max_digits10);
        if (converted.ec != std::errc{}) throw JsonError("Number serialization failed");
        out.append(buffer, converted.ptr);
    } else if (value.isString()) quote(out, value.asString());
    else if (value.isArray()) {
        out.push_back('[');
        bool first = true;
        for (const auto& item : value.asArray()) {
            if (!first) out.push_back(',');
            first = false; serialize(out, item, depth + 1);
        }
        out.push_back(']');
    } else {
        out.push_back('{');
        bool first = true;
        for (const auto& item : value.asObject()) {
            if (!first) out.push_back(',');
            first = false; quote(out, item.first); out.push_back(':'); serialize(out, item.second, depth + 1);
        }
        out.push_back('}');
    }
}

} // namespace

bool isValidUtf8(std::string_view value) noexcept {
    for (std::size_t i = 0; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i++]);
        if (first < 0x80) continue;
        unsigned cp = 0, minimum = 0;
        std::size_t count = 0;
        if (first >= 0xc2 && first <= 0xdf) { cp = first & 31; minimum = 0x80; count = 1; }
        else if (first >= 0xe0 && first <= 0xef) { cp = first & 15; minimum = 0x800; count = 2; }
        else if (first >= 0xf0 && first <= 0xf4) { cp = first & 7; minimum = 0x10000; count = 3; }
        else return false;
        if (count > value.size() - i) return false;
        for (std::size_t n = 0; n < count; ++n) {
            const auto continuation = static_cast<unsigned char>(value[i++]);
            if ((continuation & 0xc0) != 0x80) return false;
            cp = cp * 64 + (continuation & 63);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    }
    return true;
}

bool Json::isNull() const noexcept { return std::holds_alternative<std::nullptr_t>(value_); }
bool Json::isBool() const noexcept { return std::holds_alternative<bool>(value_); }
bool Json::isNumber() const noexcept { return std::holds_alternative<double>(value_); }
bool Json::isString() const noexcept { return std::holds_alternative<std::string>(value_); }
bool Json::isArray() const noexcept { return std::holds_alternative<Array>(value_); }
bool Json::isObject() const noexcept { return std::holds_alternative<Object>(value_); }
bool Json::asBool() const { if (!isBool()) throw JsonError("Expected boolean"); return std::get<bool>(value_); }
double Json::asNumber() const { if (!isNumber()) throw JsonError("Expected number"); return std::get<double>(value_); }
const std::string& Json::asString() const { if (!isString()) throw JsonError("Expected string"); return std::get<std::string>(value_); }
const Json::Array& Json::asArray() const { if (!isArray()) throw JsonError("Expected array"); return std::get<Array>(value_); }
Json::Array& Json::asArray() { if (!isArray()) throw JsonError("Expected array"); return std::get<Array>(value_); }
const Json::Object& Json::asObject() const { if (!isObject()) throw JsonError("Expected object"); return std::get<Object>(value_); }
Json::Object& Json::asObject() { if (!isObject()) throw JsonError("Expected object"); return std::get<Object>(value_); }
const Json* Json::get(std::string_view key) const noexcept {
    if (!isObject()) return nullptr;
    const auto& object = std::get<Object>(value_);
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &found->second;
}
bool Json::contains(std::string_view key) const noexcept { return get(key) != nullptr; }
const Json& Json::at(std::string_view key) const { const auto* member = get(key); if (!member) throw JsonError("Missing object member"); return *member; }
Json& Json::operator[](const std::string& key) { if (isNull()) value_ = Object{}; return asObject()[key]; }
std::string Json::stringify() const { std::string result; serialize(result, *this, 0); return result; }
Json Json::parse(std::string_view input, std::size_t maxDepth) { return Parser(input, maxDepth).run(); }

} // namespace hikari

// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace hikari {

class JsonError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json, std::less<>>;
    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool value) : value_(value) {}
    template <class T, std::enable_if_t<std::is_arithmetic_v<T> && !std::is_same_v<T, bool>, int> = 0>
    Json(T value) : value_(static_cast<double>(value)) {}
    // An enum would otherwise convert to bool and be stored as true or false; callers convert it to a number themselves.
    template <class T, std::enable_if_t<std::is_enum_v<T>, long> = 0>
    Json(T value) = delete;
    Json(const char* value) : value_(std::string(value)) {}
    Json(std::string value) : value_(std::move(value)) {}
    Json(std::string_view value) : value_(std::string(value)) {}
    Json(Array value) : value_(std::move(value)) {}
    Json(Object value) : value_(std::move(value)) {}

    bool isNull() const noexcept;
    bool isBool() const noexcept;
    bool isNumber() const noexcept;
    bool isString() const noexcept;
    bool isArray() const noexcept;
    bool isObject() const noexcept;
    bool asBool() const;
    double asNumber() const;
    const std::string& asString() const;
    const Array& asArray() const;
    Array& asArray();
    const Object& asObject() const;
    Object& asObject();
    const Json* get(std::string_view key) const noexcept;
    bool contains(std::string_view key) const noexcept;
    const Json& at(std::string_view key) const;
    Json& operator[](const std::string& key);
    std::string stringify() const;
    static Json parse(std::string_view input, std::size_t maxDepth = 64);
    static Json object(Object value = {}) { return Json(std::move(value)); }
    static Json array(Array value = {}) { return Json(std::move(value)); }

private:
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> value_{nullptr};
};

bool isValidUtf8(std::string_view value) noexcept;

} // namespace hikari

// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include <windows.h>
#include <string>
namespace hikari {
class PrivateSecurity {
public:
    PrivateSecurity();
    ~PrivateSecurity();
    SECURITY_ATTRIBUTES* get() { return &attributes_; }
private:
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    SECURITY_ATTRIBUTES attributes_{};
};
class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
private:
    HANDLE value_;
};
std::wstring quoteWindows(const std::wstring& value);
std::wstring executablePath();
std::string processCreation(HANDLE process);
}

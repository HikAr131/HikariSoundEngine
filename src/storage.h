// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include "json.h"
#include <filesystem>
#include <string>
#include <mutex>

namespace hikari {
class Storage {
public:
    explicit Storage(std::filesystem::path root);
    Json read(const std::string& name, bool missingAllowed = true) const;
    void writeState(const Json& state);
    void log(const std::string& message) noexcept;
    const std::filesystem::path& root() const { return root_; }
private:
    std::filesystem::path root_;
    std::mutex logMutex_;
};
unsigned long long unixMillis();
}

// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include "json.h"
#include <filesystem>
#include <string>
namespace hikari {
struct RunOptions {
    std::filesystem::path dataDir;
    std::wstring captureId;
    std::wstring outputId;
    bool noDefaultSwitch = false;
};
struct Endpoint;
// Recovery role records for a start that found no real default (console and multimedia, both to this device).
Json::Array fallbackRoleRecords(const Endpoint& endpoint);
int runEngine(const RunOptions& options);
int runGuard(const RunOptions& options, unsigned long pid, const std::string& creation, const std::string& instance);
Json probeDevices();
}

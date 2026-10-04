// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include <string>
#include <vector>

namespace hikari {
struct Endpoint {
    std::wstring id;
    std::string name;
    bool virtualDevice = false;
    bool consoleDefault = false;
    bool multimediaDefault = false;
    unsigned channels = 0;
    unsigned sampleRate = 0;
    bool formatSupported = false;
    float volume = 1;
    bool muted = false;
    bool volumeKnown = false;
};
std::string utf8(const std::wstring& value);
std::wstring wide(const std::string& value);
std::vector<Endpoint> enumerateEndpoints();
std::wstring defaultEndpoint(unsigned role);
bool setDefaultEndpoint(const std::wstring& id, unsigned role);
bool restoreVolume(const std::wstring& id, float volume, bool muted);
long probeSharedOutputInitialize(const std::wstring& endpointId) noexcept;
bool officialFxSoundRunning();
}

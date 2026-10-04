// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include <cstddef>
#include <cwchar>

namespace hikari {
inline bool hasFxVadHardwareId(const wchar_t* value, std::size_t characters) noexcept {
    if (!value || characters < 2 || value[characters - 1] || value[characters - 2]) return false;
    for (std::size_t start = 0; start + 1 < characters && value[start];) {
        std::size_t end = start;
        while (end < characters && value[end]) ++end;
        if (end == characters) return false;
        if (_wcsicmp(value + start, L"Root\\FXVAD") == 0 || _wcsicmp(value + start, L"*FXVAD") == 0) return true;
        start = end + 1;
    }
    return false;
}
void runAudioIdentityTests();
}

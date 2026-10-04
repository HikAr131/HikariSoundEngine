// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "audio_identity.h"
#include <stdexcept>

namespace hikari {
void runAudioIdentityTests() {
    const wchar_t good[] = {L'R', L'o', L'o', L't', L'\\', L'F', L'X', L'V', L'A', L'D', 0, 0};
    const wchar_t alias[] = {L'*', L'f', L'x', L'v', L'a', L'd', 0, 0};
    const wchar_t fake[] = {L'F', L'x', L'S', L'o', L'u', L'n', L'd', 0, 0};
    const wchar_t suffix[] = {L'*', L'F', L'X', L'V', L'A', L'D', L'2', 0, 0};
    if (!hasFxVadHardwareId(good, sizeof(good) / sizeof(*good)) || !hasFxVadHardwareId(alias, sizeof(alias) / sizeof(*alias)) ||
        hasFxVadHardwareId(fake, sizeof(fake) / sizeof(*fake)) || hasFxVadHardwareId(suffix, sizeof(suffix) / sizeof(*suffix)) ||
        hasFxVadHardwareId(good, 10) || hasFxVadHardwareId(nullptr, 2)) throw std::runtime_error("Audio hardware identity tests failed");
}
}

// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>

namespace hikari {
bool validateAudioFormat(const WAVEFORMATEX* format) noexcept;
bool matchesCachedAudioFormat(const WAVEFORMATEX* format, const WAVEFORMATEX& cached) noexcept;
void runAudioFormatTests();
}

bool hikariValidateAudioInitialization(const WAVEFORMATEX* format, const WAVEFORMATEX* cached) noexcept;

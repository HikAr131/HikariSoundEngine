// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include <windows.h>
#include <mmsystem.h>
#include "hikari_upstream_logging.h"
class DfxDsp;
int hikariProcessAudio(DfxDsp*, float*, int, int, int, int, int, int);
bool hikariAllowDefaultSwitch(const wchar_t*);
void hikariOnDefaultDeviceChanged(int, int, const wchar_t*);
bool hikariGetPreferredOutput(wchar_t* buffer, int capacity);
bool hikariValidateAudioInitialization(const WAVEFORMATEX*, const WAVEFORMATEX*) noexcept;
LSTATUS WINAPI hikariRegCreateKeyExW(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, REGSAM,
    const LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);
LSTATUS WINAPI hikariRegOpenKeyExW(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
LSTATUS WINAPI hikariRegDeleteKeyW(HKEY, LPCWSTR);

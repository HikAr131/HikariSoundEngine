// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "dsp_adapter.h"
#include "hikari_upstream_logging.h"
#include <windows.h>
#include <cstdio>
#include <exception>
bool hikariAllowDefaultSwitch(const wchar_t*) { return false; }
void hikariOnDefaultDeviceChanged(int, int, const wchar_t*) {}
bool hikariGetPreferredOutput(wchar_t*, int) { return false; }
void hikariOnPlaybackInitializeResult(HRESULT) noexcept {}
extern "C" void hikariLogUpstreamError(const wchar_t* message) {
    if (message) std::fwprintf(stderr,L"%s\n",message);
}
int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    try { hikari::runDspTests(); std::puts("Offline upstream DSP tests passed"); return 0; }
    catch (const std::exception& error) { std::fprintf(stderr,"%s\n",error.what()); return 1; }
}

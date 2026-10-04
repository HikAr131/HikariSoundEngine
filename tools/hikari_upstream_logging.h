// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include <wchar.h>
#ifdef __cplusplus
extern "C" {
#endif
void hikariLogUpstreamError(const wchar_t* message);
void hikariLogUpstreamErrorA(const char* message);
#ifdef __cplusplus
}
#endif

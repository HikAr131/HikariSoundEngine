// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include "json.h"
#include "parameters.h"
namespace hikari {
Parameters parseParameters(const Json& input);
Json parametersJson(const Parameters& parameters);
// The compare-with-original hold is momentary: bypass is never persisted and never restored.
Json persistedParametersJson(const Parameters& parameters);
Parameters restoredParameters(const Json& input);
}

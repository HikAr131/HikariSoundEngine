// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#pragma once
#include "json.h"
#include "parameters.h"
namespace hikari {
Parameters parseParameters(const Json& input);
Json parametersJson(const Parameters& parameters);
}

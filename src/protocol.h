// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "json.h"

namespace hikari {

inline constexpr std::size_t kMaxJsonLineBytes = 64 * 1024;
Json parseRequest(std::string_view line);
Json badRequest(const Json* id = nullptr, std::string message = "Invalid request");
Json protocolError(std::string code, std::string message, const Json* id = nullptr);
Json attachRequestId(Json response, const Json& request);

} // namespace hikari

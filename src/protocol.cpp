// SPDX-License-Identifier: AGPL-3.0-or-later
#include "protocol.h"

#include <cmath>

namespace hikari {
namespace {
bool validId(const Json& id) {
    if (id.isString()) return id.asString().size() <= 128;
    if (!id.isNumber()) return false;
    const auto number = id.asNumber();
    return std::isfinite(number) && std::floor(number) == number && std::abs(number) <= 9007199254740991.0;
}
bool validEndpoint(const Json* id) {
    if (!id || !id->isString() || id->asString().empty() || id->asString().size() > 4096) return false;
    for (const unsigned char ch : id->asString()) if (ch < 0x20 || ch == 0x7f) return false;
    return true;
}
} // namespace

Json parseRequest(std::string_view line) {
    if (line.size() > kMaxJsonLineBytes) throw JsonError("Request line exceeds 64 KB");
    auto request = Json::parse(line);
    if (!request.isObject()) throw JsonError("Request must be an object");
    if (const auto* id = request.get("id"); id && !validId(*id)) throw JsonError("Invalid request id");
    const auto* cmdValue = request.get("cmd");
    if (!cmdValue || !cmdValue->isString()) throw JsonError("Missing command");
    const auto& cmd = cmdValue->asString();
    if (cmd == "hello" || cmd == "status" || cmd == "devices" || cmd == "subscribe" || cmd == "quit") return request;
    if (cmd == "apply") {
        const auto* params = request.get("params");
        if (!params || !params->isObject()) throw JsonError("Apply requires an object");
    } else if (cmd == "set-output") {
        const auto* mode = request.get("mode");
        if (!mode || !mode->isString() || (mode->asString() != "follow" && mode->asString() != "fixed"))
            throw JsonError("Invalid output mode");
        if (mode->asString() == "fixed" && !validEndpoint(request.get("deviceId"))) throw JsonError("Invalid output id");
    } else if (cmd == "set-buffer") {
        const auto* ms = request.get("ms");
        if (!ms || !ms->isNumber() || !std::isfinite(ms->asNumber())) throw JsonError("Buffer duration must be finite");
    } else throw JsonError("Unknown command");
    return request;
}

Json protocolError(std::string code, std::string message, const Json* id) {
    Json result = Json::Object{{"ok", false}, {"code", std::move(code)}, {"message", std::move(message)}};
    if (id && validId(*id)) result["id"] = *id;
    return result;
}
Json badRequest(const Json* id, std::string message) { return protocolError("BAD_REQUEST", std::move(message), id); }
Json attachRequestId(Json response, const Json& request) {
    if (!response.isObject()) throw JsonError("Response must be an object");
    if (const auto* id = request.get("id")) response["id"] = *id;
    return response;
}

} // namespace hikari

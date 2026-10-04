// SPDX-License-Identifier: AGPL-3.0-or-later
#include "json.h"
#include "protocol.h"
#include "pipe_server.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace hikari;

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template <class F> void rejects(F operation, const char* message) {
    try { operation(); } catch (const JsonError&) { return; }
    throw std::runtime_error(message);
}

class TestHandle {
public:
    explicit TestHandle(HANDLE value) : value_(value) {}
    ~TestHandle() { if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    HANDLE get() const { return value_; }
private:
    HANDLE value_;
};

HANDLE openTestClient(const std::wstring& name) {
    for (int retry = 0; retry < 50; ++retry) {
        const auto pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) return pipe;
        if (GetLastError() != ERROR_PIPE_BUSY) break;
        WaitNamedPipeW(name.c_str(), 50);
    }
    throw std::runtime_error("Could not open offline test pipe");
}

DWORD testIo(HANDLE pipe, bool write, char* buffer, DWORD size) {
    TestHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    require(event.get() != nullptr, "Test I/O event failed");
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    DWORD transferred = 0;
    const BOOL finished = write ? WriteFile(pipe, buffer, size, &transferred, &operation)
                                : ReadFile(pipe, buffer, size, &transferred, &operation);
    if (!finished) {
        const auto error = GetLastError();
        if (error != ERROR_IO_PENDING)
            throw std::runtime_error(std::string("Test pipe ") + (write ? "write" : "read") + " failed: " + std::to_string(error));
        const auto waited = WaitForSingleObject(event.get(), 3000);
        if (waited != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &operation);
            GetOverlappedResult(pipe, &operation, &transferred, TRUE);
            throw std::runtime_error("Offline test I/O exceeded deadline");
        }
        require(GetOverlappedResult(pipe, &operation, &transferred, FALSE) != FALSE, "Test I/O completion failed");
    }
    return transferred;
}

void sendTestLine(HANDLE pipe, std::string line) {
    line.push_back('\n');
    std::size_t sent = 0;
    while (sent < line.size()) {
        const auto bytes = testIo(pipe, true, line.data() + sent, static_cast<DWORD>(line.size() - sent));
        require(bytes > 0, "Test write made no progress");
        sent += bytes;
    }
}

Json readTestLine(HANDLE pipe) {
    std::string line;
    while (line.size() <= kMaxJsonLineBytes) {
        char ch = 0;
        require(testIo(pipe, false, &ch, 1) == 1, "Test read made no progress");
        if (ch == '\n') return Json::parse(line);
        line.push_back(ch);
    }
    throw std::runtime_error("Oversized test response");
}

void pipeSmoke() {
    const auto name = currentSessionPipeName() + L".selftest." + std::to_wstring(GetCurrentProcessId());
    PipeServer server([](const Json& request) {
        if (request.at("cmd").asString() == "hello") return Json::object({{"ok", true}, {"protocol", 1}});
        return Json::object({{"ok", true}, {"state", "offline-test"}});
    }, name);
    server.start();
    const auto reply = PipeServer::requestOnce(Json::object({{"id", 7}, {"cmd", "hello"}}), 2000, name);
    require(reply.at("ok").asBool() && reply.at("id").asNumber() == 7 && reply.at("protocol").asNumber() == 1,
            "Offline pipe hello failed");
    bool duplicateRejected = false;
    try {
        PipeServer duplicate([](const Json&) { return Json::object({{"ok", true}}); }, name);
        duplicate.start();
    } catch (const std::exception&) { duplicateRejected = true; }
    require(duplicateRejected, "FIRST_PIPE_INSTANCE must reject occupied name");
    {
        TestHandle client(openTestClient(name));
        sendTestLine(client.get(), R"({"id":"bad-command","cmd":"unknown"})");
        const auto unknown = readTestLine(client.get());
        require(unknown.at("code").asString() == "BAD_REQUEST" && unknown.at("id").asString() == "bad-command",
                "Unknown command must return BAD_REQUEST with id");
        sendTestLine(client.get(), R"({"id":11,"cmd":"status","cmd":"quit"})");
        require(readTestLine(client.get()).at("code").asString() == "BAD_REQUEST", "Duplicate-key request was dispatched");
        sendTestLine(client.get(), R"({"cmd":"subscribe"})");
        require(readTestLine(client.get()).at("ok").asBool(), "Subscription failed");
        server.broadcast(Json::object({{"event", "state"}, {"state", "offline-test"}}));
        require(readTestLine(client.get()).at("event").asString() == "state", "Subscription did not receive event");
    }
    {
        TestHandle oversized(openTestClient(name));
        std::string line(kMaxJsonLineBytes + 1, ' ');
        sendTestLine(oversized.get(), line);
        require(readTestLine(oversized.get()).at("code").asString() == "BAD_REQUEST", "Oversized wire line must return BAD_REQUEST");
    }
    TestHandle idle(openTestClient(name));
    TestHandle partial(openTestClient(name));
    std::string incomplete = R"({"cmd":"status")";
    require(testIo(partial.get(), true, incomplete.data(), static_cast<DWORD>(incomplete.size())) == incomplete.size(),
            "Partial-frame setup failed");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const auto start = std::chrono::steady_clock::now();
    server.stop();
    require(std::chrono::steady_clock::now() - start < std::chrono::seconds(2), "Idle or partial client prevented pipe cancellation");
}

} // namespace

void runProtocolTests() {
    using namespace hikari;
    const auto value = Json::parse(R"({"text":"hello\nworld","list":[null,true,false,-1.5e2],"unicode":"\ud83c\udfa7"})");
    require(value.at("text").asString() == "hello\nworld", "String escape mismatch");
    require(value.at("list").asArray().at(3).asNumber() == -150, "Number parse mismatch");
    require(Json::parse(value.stringify()).stringify() == value.stringify(), "JSON round trip failed");
    require(value.at("unicode").asString().size() == 4, "Surrogate pair encoding failed");
    const std::vector<std::string> malformed = {
        "", "[", "{}{}", "{\"a\":1,\"a\":2}", "{\"a\":1,\"\\u0061\":2}", "[1,]", "{\"x\":true,}",
        "01", "+1", "1.", "1e", "1e+", "NaN", "Infinity", "1e999", "truex", "\"\\x\"",
        "\"\\ud800\"", "\"\\udc00\"", "\"\\ud800\\u0041\""
    };
    for (const auto& bad : malformed) rejects([&] { Json::parse(bad); }, "Malformed JSON was accepted");
    std::string control = "\"a";
    control.push_back(static_cast<char>(1)); control += "b\"";
    rejects([&] { Json::parse(control); }, "Unescaped control accepted");
    std::string overlong = "\"";
    overlong.push_back(static_cast<char>(0xc0)); overlong.push_back(static_cast<char>(0xaf)); overlong += "\"";
    rejects([&] { Json::parse(overlong); }, "Overlong UTF-8 accepted");
    std::string surrogate = "\"";
    surrogate.push_back(static_cast<char>(0xed)); surrogate.push_back(static_cast<char>(0xa0));
    surrogate.push_back(static_cast<char>(0x80)); surrogate += "\"";
    rejects([&] { Json::parse(surrogate); }, "UTF-8 surrogate accepted");
    const auto deep = std::string(65, '[') + "0" + std::string(65, ']');
    rejects([&] { Json::parse(deep); }, "Depth limit missing");
    rejects([] { Json(std::numeric_limits<double>::infinity()).stringify(); }, "Nonfinite output accepted");
    const auto allControls = Json(std::string(1, static_cast<char>(0)));
    require(Json::parse(allControls.stringify()).asString() == allControls.asString(), "Control escape round trip failed");

    const std::vector<std::string> validRequests = {
        R"({"id":1,"cmd":"hello"})", R"({"cmd":"status"})", R"({"cmd":"devices"})", R"({"cmd":"subscribe"})",
        R"({"cmd":"quit"})", R"({"cmd":"apply","params":{}})", R"({"cmd":"set-buffer","ms":40})",
        R"({"cmd":"set-output","mode":"follow"})", R"({"cmd":"set-output","mode":"fixed","deviceId":"endpoint"})"
    };
    for (const auto& request : validRequests) require(parseRequest(request).isObject(), "Valid request rejected");
    const std::vector<std::string> invalidRequests = {
        "null", "[]", "{}", R"({"cmd":1})", R"({"cmd":"unknown"})", R"({"cmd":"apply"})",
        R"({"cmd":"apply","params":[]})", R"({"cmd":"set-buffer","ms":"40"})",
        R"({"cmd":"set-output","mode":"fixed"})", R"({"cmd":"set-output","mode":"other"})",
        R"({"cmd":"status","id":true})", R"({"cmd":"status","id":null})", R"({"cmd":"status","id":1.5})",
        R"({"cmd":"status","id":9007199254740992})"
    };
    for (const auto& request : invalidRequests) rejects([&] { parseRequest(request); }, "Malformed request accepted");
    rejects([] { parseRequest(std::string(kMaxJsonLineBytes + 1, ' ')); }, "Oversized request accepted");
    rejects([] { parseRequest(Json::object({{"cmd", "status"}, {"id", std::string(129, 'a')}}).stringify()); },
            "Oversized id accepted");
    require(badRequest(nullptr).at("code").asString() == "BAD_REQUEST", "Error code mismatch");
    pipeSmoke();
}

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pipe_server.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

namespace hikari {
namespace {

class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.release()) {}
    Handle& operator=(Handle&& other) noexcept { if (this != &other) reset(other.release()); return *this; }
    HANDLE get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ && value_ != INVALID_HANDLE_VALUE; }
    HANDLE release() noexcept { const auto value = value_; value_ = nullptr; return value; }
    void reset(HANDLE value = nullptr) noexcept {
        if (*this) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_;
};

[[noreturn]] void winError(const char* message) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), message);
}

class PipeSecurity {
public:
    PipeSecurity() {
        HANDLE rawToken = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) winError("OpenProcessToken");
        Handle token(rawToken);
        DWORD size = 0;
        GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
        if (!size) winError("GetTokenInformation");
        std::vector<unsigned char> buffer(size);
        if (!GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size)) winError("GetTokenInformation");
        const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
        LPWSTR rawSid = nullptr;
        if (!ConvertSidToStringSidW(user->User.Sid, &rawSid)) winError("ConvertSidToStringSid");
        const std::wstring sid(rawSid);
        LocalFree(rawSid);
        const auto sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + sid + L")";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor_, nullptr))
            winError("Create pipe security descriptor");
        attributes_.nLength = sizeof(attributes_);
        attributes_.lpSecurityDescriptor = descriptor_;
        attributes_.bInheritHandle = FALSE;
    }
    ~PipeSecurity() { if (descriptor_) LocalFree(descriptor_); }
    SECURITY_ATTRIBUTES* attributes() noexcept { return &attributes_; }
private:
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    SECURITY_ATTRIBUTES attributes_{};
};

enum class IoResult { Complete, Stopped, Timeout, Closed };

IoResult finishIo(HANDLE pipe, OVERLAPPED& operation, HANDLE stop, DWORD timeout, DWORD& transferred) {
    const HANDLE waits[] = {stop, operation.hEvent};
    const auto count = stop ? 2UL : 1UL;
    const auto result = WaitForMultipleObjects(count, stop ? waits : waits + 1, FALSE, timeout);
    const auto completedIndex = stop ? WAIT_OBJECT_0 + 1 : WAIT_OBJECT_0;
    if (result != completedIndex) {
        CancelIoEx(pipe, &operation);
        // The OVERLAPPED storage must live until cancellation completes.
        GetOverlappedResult(pipe, &operation, &transferred, TRUE);
        return result == WAIT_TIMEOUT ? IoResult::Timeout : IoResult::Stopped;
    }
    return GetOverlappedResult(pipe, &operation, &transferred, FALSE) ? IoResult::Complete : IoResult::Closed;
}

IoResult transfer(HANDLE pipe, HANDLE stop, bool write, char* bytes, DWORD size, DWORD timeout, DWORD& transferred) {
    if (stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return IoResult::Stopped;
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) winError("Create pipe I/O event");
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    transferred = 0;
    const BOOL completed = write ? WriteFile(pipe, bytes, size, &transferred, &operation)
                                 : ReadFile(pipe, bytes, size, &transferred, &operation);
    if (completed) return IoResult::Complete;
    if (GetLastError() != ERROR_IO_PENDING) return IoResult::Closed;
    return finishIo(pipe, operation, stop, timeout, transferred);
}

DWORD remaining(ULONGLONG deadline) {
    if (deadline == 0) return INFINITE;
    const auto now = GetTickCount64();
    return now >= deadline ? 0 : static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, MAXDWORD - 1));
}

bool writeLine(HANDLE pipe, HANDLE stop, const std::string& line, unsigned timeoutMs) {
    if (line.size() > kMaxJsonLineBytes) return false;
    std::string wire = line;
    wire.push_back('\n');
    const auto deadline = GetTickCount64() + timeoutMs;
    std::size_t sent = 0;
    while (sent < wire.size()) {
        const auto timeout = remaining(deadline);
        if (timeout == 0) return false;
        DWORD transferred = 0;
        if (transfer(pipe, stop, true, wire.data() + sent, static_cast<DWORD>(wire.size() - sent), timeout, transferred)
            != IoResult::Complete || !transferred) return false;
        sent += transferred;
    }
    return true;
}

enum class LineResult { Complete, Closed, Oversized, Timeout };

LineResult readLine(HANDLE pipe, HANDLE stop, std::string& pending, std::string& line,
                    ULONGLONG totalDeadline = 0) {
    auto frameDeadline = pending.empty() ? 0 : GetTickCount64() + 2000;
    for (;;) {
        const auto newline = pending.find('\n');
        if (newline != std::string::npos) {
            if (newline > kMaxJsonLineBytes) return LineResult::Oversized;
            line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return LineResult::Complete;
        }
        if (pending.size() > kMaxJsonLineBytes) return LineResult::Oversized;
        auto timeout = remaining(totalDeadline);
        if (frameDeadline) timeout = std::min(timeout, remaining(frameDeadline));
        if (!timeout) return LineResult::Timeout;
        char buffer[4096];
        DWORD transferred = 0;
        const auto result = transfer(pipe, stop, false, buffer, sizeof(buffer), timeout, transferred);
        if (result == IoResult::Timeout) return LineResult::Timeout;
        if (result != IoResult::Complete || transferred == 0) return LineResult::Closed;
        if (pending.empty()) frameDeadline = GetTickCount64() + 2000;
        pending.append(buffer, transferred);
    }
}

} // namespace

std::wstring currentSessionPipeName() {
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) winError("Resolve current session");
    return L"\\\\.\\pipe\\Hikari1U.SoundEngine." + std::to_wstring(session);
}

class PipeServer::Impl {
public:
    struct Client {
        Handle pipe;
        std::mutex writeMutex;
        std::atomic<bool> subscribed{false};
        std::atomic<bool> done{false};
        std::thread worker;
        explicit Client(Handle handle) : pipe(std::move(handle)) {}
        ~Client() { if (worker.joinable()) worker.join(); }
    };

    Impl(Handler callback, std::wstring pipeName) : handler(std::move(callback)), name(std::move(pipeName)) {
        if (!handler || name.rfind(L"\\\\.\\pipe\\Hikari1U.SoundEngine.", 0) != 0)
            throw std::invalid_argument("Invalid pipe server configuration");
    }
    ~Impl() { stopServer(); }

    Handle createPipe(bool first) {
        auto access = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
        if (first) access |= FILE_FLAG_FIRST_PIPE_INSTANCE;
        Handle pipe(CreateNamedPipeW(name.c_str(), access, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                        PIPE_REJECT_REMOTE_CLIENTS, 9, 65536, 65536, 0, security.attributes()));
        if (!pipe) winError("Create named pipe");
        return pipe;
    }

    void startServer() {
        if (acceptThread.joinable()) throw std::logic_error("Pipe server already started");
        stopEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!stopEvent) winError("Create pipe stop event");
        auto first = createPipe(true);
        acceptThread = std::thread([this, first = std::move(first)]() mutable { acceptLoop(std::move(first)); });
    }

    void stopServer() noexcept {
        if (stopEvent) SetEvent(stopEvent.get());
        if (acceptThread.joinable()) acceptThread.join();
        std::vector<std::shared_ptr<Client>> retired;
        { std::lock_guard<std::mutex> lock(clientsMutex); retired.swap(clients); }
        for (const auto& client : retired) if (client->worker.joinable()) client->worker.join();
        stopEvent.reset();
    }

    void reapClients() {
        std::vector<std::shared_ptr<Client>> retired;
        {
            std::lock_guard<std::mutex> lock(clientsMutex);
            for (auto it = clients.begin(); it != clients.end();) {
                if ((*it)->done) { retired.push_back(*it); it = clients.erase(it); }
                else ++it;
            }
        }
        for (const auto& client : retired) if (client->worker.joinable()) client->worker.join();
    }

    void acceptLoop(Handle next) noexcept {
        try {
            while (WaitForSingleObject(stopEvent.get(), 0) != WAIT_OBJECT_0) {
                Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                if (!event) winError("Create accept event");
                OVERLAPPED operation{};
                operation.hEvent = event.get();
                const BOOL connected = ConnectNamedPipe(next.get(), &operation);
                if (!connected) {
                    const auto error = GetLastError();
                    if (error == ERROR_IO_PENDING) {
                        DWORD ignored = 0;
                        if (finishIo(next.get(), operation, stopEvent.get(), INFINITE, ignored) != IoResult::Complete) break;
                    } else if (error != ERROR_PIPE_CONNECTED) break;
                }
                reapClients();
                bool full = false;
                { std::lock_guard<std::mutex> lock(clientsMutex); full = clients.size() >= 8; }
                if (full) {
                    if (writeLine(next.get(), stopEvent.get(), protocolError("INTERNAL", "Too many clients").stringify(), 100))
                        drainAfterTerminalReply(next.get());
                    DisconnectNamedPipe(next.get());
                    continue;
                }
                // Keep an instance open while creating its successor, retaining ownership of the name.
                auto successor = createPipe(false);
                auto client = std::make_shared<Client>(std::move(next));
                { std::lock_guard<std::mutex> lock(clientsMutex); clients.push_back(client); }
                client->worker = std::thread([this, client] { serve(client); });
                next = std::move(successor);
            }
        } catch (...) {
            SetEvent(stopEvent.get());
        }
    }

    bool send(const std::shared_ptr<Client>& client, const Json& response, bool subscribe = false) {
        std::string line;
        try { line = response.stringify(); }
        catch (const JsonError&) { line = protocolError("INTERNAL", "Invalid command response", response.get("id")).stringify(); }
        if (line.size() > kMaxJsonLineBytes)
            line = protocolError("INTERNAL", "Response exceeds protocol limit", response.get("id")).stringify();
        std::lock_guard<std::mutex> lock(client->writeMutex);
        if (client->done) return false;
        if (subscribe) client->subscribed = true;
        const auto sent = writeLine(client->pipe.get(), stopEvent.get(), line, 500);
        if (subscribe && !sent) client->subscribed = false;
        return sent;
    }

    void drainAfterTerminalReply(HANDLE pipe) {
        // DisconnectNamedPipe discards unread replies; wait for peer closure with a fixed budget.
        const auto deadline = GetTickCount64() + 500;
        while (const auto timeout = remaining(deadline)) {
            char discarded[4096];
            DWORD transferred = 0;
            if (transfer(pipe, stopEvent.get(), false, discarded, sizeof(discarded), timeout, transferred)
                != IoResult::Complete || transferred == 0) break;
        }
    }

    void serve(const std::shared_ptr<Client>& client) noexcept {
        try {
            std::string pending, line;
            while (WaitForSingleObject(stopEvent.get(), 0) != WAIT_OBJECT_0) {
                const auto result = readLine(client->pipe.get(), stopEvent.get(), pending, line);
                if (result == LineResult::Oversized || result == LineResult::Timeout) {
                    const auto message = result == LineResult::Oversized ? "Request line exceeds 64 KB" : "Incomplete request line";
                    if (send(client, badRequest(nullptr, message))) drainAfterTerminalReply(client->pipe.get());
                    break;
                }
                if (result != LineResult::Complete) break;
                Json request;
                Json response;
                bool subscribeAfterReply = false;
                try {
                    request = parseRequest(line);
                    response = handler(request);
                    response = attachRequestId(std::move(response), request);
                    const auto* ok = response.get("ok");
                    if (request.at("cmd").asString() == "subscribe" && ok && ok->isBool() && ok->asBool())
                        subscribeAfterReply = true;
                } catch (const JsonError& error) {
                    const Json* id = request.get("id");
                    if (!id) {
                        try { request = Json::parse(line); id = request.get("id"); } catch (...) {}
                    }
                    response = badRequest(id, error.what());
                } catch (...) {
                    response = protocolError("INTERNAL", "Command could not be completed", request.get("id"));
                }
                if (!send(client, response, subscribeAfterReply)) break;
            }
        } catch (...) {}
        client->subscribed = false;
        { std::lock_guard<std::mutex> lock(client->writeMutex); client->done = true; DisconnectNamedPipe(client->pipe.get()); }
    }

    void broadcastEvent(const Json& event) {
        if (!event.isObject() || !event.get("event") || !event.at("event").isString())
            throw JsonError("Broadcast requires an event object");
        if (event.stringify().size() > kMaxJsonLineBytes) throw JsonError("Event exceeds protocol limit");
        std::vector<std::shared_ptr<Client>> snapshot;
        { std::lock_guard<std::mutex> lock(clientsMutex); snapshot = clients; }
        for (const auto& client : snapshot) {
            if (client->subscribed && !client->done && !send(client, event)) {
                client->subscribed = false;
                CancelIoEx(client->pipe.get(), nullptr);
            }
        }
    }

    Handler handler;
    std::wstring name;
    PipeSecurity security;
    Handle stopEvent;
    std::thread acceptThread;
    std::mutex clientsMutex;
    std::vector<std::shared_ptr<Client>> clients;
};

PipeServer::PipeServer(Handler handler, std::wstring name) : impl_(std::make_unique<Impl>(std::move(handler), std::move(name))) {}
PipeServer::~PipeServer() = default;
void PipeServer::start() { impl_->startServer(); }
void PipeServer::stop() noexcept { impl_->stopServer(); }
void PipeServer::broadcast(const Json& event) { impl_->broadcastEvent(event); }

Json PipeServer::requestOnce(const Json& request, unsigned timeoutMs, const std::wstring& name) {
    timeoutMs = std::clamp(timeoutMs, 1U, 30000U);
    const auto deadline = GetTickCount64() + timeoutMs;
    const auto line = request.stringify();
    parseRequest(line);
    Handle pipe;
    for (;;) {
        pipe.reset(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
        if (pipe) break;
        if (GetLastError() != ERROR_PIPE_BUSY) winError("Connect engine pipe");
        const auto timeout = remaining(deadline);
        if (!timeout || !WaitNamedPipeW(name.c_str(), timeout)) winError("Wait for engine pipe");
    }
    if (!writeLine(pipe.get(), nullptr, line, remaining(deadline))) throw std::runtime_error("Pipe request timed out");
    std::string pending, response;
    if (readLine(pipe.get(), nullptr, pending, response, deadline) != LineResult::Complete)
        throw std::runtime_error("Pipe response unavailable or timed out");
    auto parsed = Json::parse(response);
    if (!parsed.isObject()) throw JsonError("Pipe response must be an object");
    return parsed;
}

} // namespace hikari

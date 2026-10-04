// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "storage.h"
#include <windows.h>
#include <fstream>
#include <chrono>
#include <stdexcept>

namespace hikari {
unsigned long long unixMillis() {
    return static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}
Storage::Storage(std::filesystem::path root) : root_(std::move(root)) {}
Json Storage::read(const std::string& name, bool missingAllowed) const {
    if (name != "state.json" && name != "launch-hint.json") throw std::runtime_error("Invalid storage name");
    const auto path = root_ / name;
    std::error_code ec;
    bool exists = std::filesystem::exists(path, ec);
    if (ec) throw std::runtime_error("State metadata read failed");
    if (!exists && missingAllowed) return Json();
    if (std::filesystem::file_size(path) > 65536) throw std::runtime_error("State exceeds size limit");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("State read failed");
    std::string contents((std::istreambuf_iterator<char>(input)), {});
    return Json::parse(contents);
}
void Storage::writeState(const Json& state) {
    std::filesystem::create_directories(root_);
    const auto payload = state.stringify() + "\n";
    if (payload.size() > 65536) throw std::runtime_error("State exceeds size limit");
    const auto temp = root_ / "state.json.tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("State create failed");
    DWORD written = 0;
    bool ok = WriteFile(file, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr) != FALSE && written == payload.size() && FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    if (!ok) { DeleteFileW(temp.c_str()); throw std::runtime_error("State flush failed"); }
    if (!MoveFileExW(temp.c_str(), (root_ / "state.json").c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str()); throw std::runtime_error("State commit failed");
    }
}
void Storage::log(const std::string& message) noexcept {
    try {
        std::lock_guard<std::mutex> lock(logMutex_);
        auto directory = root_ / "logs";
        std::filesystem::create_directories(directory);
        auto path = directory / "engine.log";
        if (std::filesystem::exists(path) && std::filesystem::file_size(path) >= 256 * 1024) {
            auto first = directory / "engine.log.1", second = directory / "engine.log.2";
            if (std::filesystem::exists(second)) std::filesystem::remove(second);
            if (std::filesystem::exists(first)) std::filesystem::rename(first, second);
            std::filesystem::rename(path, first);
        }
        std::ofstream output(path, std::ios::binary | std::ios::app);
        std::string safe = message.substr(0, 512);
        for (auto& c : safe) if (static_cast<unsigned char>(c) < 32) c = '?';
        output << unixMillis() << " " << safe << '\n';
    } catch (...) {}
}
}

// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include "security.h"
#include <sddl.h>
#include <vector>
#include <stdexcept>

namespace hikari {
PrivateSecurity::PrivateSecurity() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) throw std::runtime_error("Token query failed");
    Handle token(raw); DWORD size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> data(size);
    if (!GetTokenInformation(token.get(), TokenUser, data.data(), size, &size)) throw std::runtime_error("User query failed");
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &sid)) throw std::runtime_error("SID query failed");
    std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor_, nullptr)) throw std::runtime_error("Security descriptor failed");
    attributes_.nLength = sizeof(attributes_); attributes_.lpSecurityDescriptor = descriptor_; attributes_.bInheritHandle = FALSE;
}
PrivateSecurity::~PrivateSecurity() { if (descriptor_) LocalFree(descriptor_); }
std::wstring quoteWindows(const std::wstring& value) {
    std::wstring result(1, L'"'); unsigned slashes = 0;
    for (auto c : value) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') result.append(slashes * 2 + 1, L'\\'); else result.append(slashes, L'\\');
        slashes = 0; result += c;
    }
    result.append(slashes * 2, L'\\'); result += L'"'; return result;
}
std::wstring executablePath() {
    std::vector<wchar_t> path(32768);
    DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!n || n >= path.size()) throw std::runtime_error("Executable path unavailable");
    return std::wstring(path.data(), n);
}
std::string processCreation(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) throw std::runtime_error("Process identity unavailable");
    ULARGE_INTEGER value; value.LowPart = created.dwLowDateTime; value.HighPart = created.dwHighDateTime;
    return std::to_string(value.QuadPart);
}
}

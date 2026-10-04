// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "audio_platform.h"
#include "audio_format.h"
#include "audio_identity.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <devicetopology.h>
#include <cfgmgr32.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <stdexcept>
#include <algorithm>
#include "PolicyConfig.h"

void hikariOnOwnDefaultWrite(unsigned role, const wchar_t* id);

namespace hikari {
using Microsoft::WRL::ComPtr;
std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (!n) throw std::runtime_error("Invalid UTF-16");
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!n) throw std::runtime_error("Invalid UTF-8");
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), out.data(), n);
    return out;
}
static ComPtr<IMMDeviceEnumerator> enumerator() {
    ComPtr<IMMDeviceEnumerator> e;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e)))) throw std::runtime_error("Audio enumeration failed");
    return e;
}
std::wstring defaultEndpoint(unsigned role) {
    auto e = enumerator(); ComPtr<IMMDevice> d;
    if (FAILED(e->GetDefaultAudioEndpoint(eRender, static_cast<ERole>(role), &d))) return {};
    LPWSTR id = nullptr;
    if (FAILED(d->GetId(&id))) return {};
    std::wstring result(id); CoTaskMemFree(id); return result;
}
static bool isFxVadEndpoint(IMMDevice* endpoint, IMMDeviceEnumerator* devices) {
    ComPtr<IDeviceTopology> topology;
    ComPtr<IConnector> connector, adapterConnector;
    ComPtr<IPart> part;
    ComPtr<IDeviceTopology> adapterTopology;
    if (FAILED(endpoint->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(topology.GetAddressOf()))) ||
        FAILED(topology->GetConnector(0, &connector)) || FAILED(connector->GetConnectedTo(&adapterConnector)) ||
        FAILED(adapterConnector.As(&part)) || FAILED(part->GetTopologyObject(&adapterTopology))) return false;
    LPWSTR adapterId = nullptr;
    if (FAILED(adapterTopology->GetDeviceId(&adapterId)) || !adapterId) return false;
    ComPtr<IMMDevice> adapter;
    const HRESULT result = devices->GetDevice(adapterId, &adapter);
    CoTaskMemFree(adapterId);
    ComPtr<IPropertyStore> properties;
    if (FAILED(result) || FAILED(adapter->OpenPropertyStore(STGM_READ, &properties))) return false;
    PROPVARIANT instance; PropVariantInit(&instance);
    DEVINST node = 0;
    bool located = SUCCEEDED(properties->GetValue(PKEY_Device_InstanceId, &instance)) && instance.vt == VT_LPWSTR && instance.pwszVal &&
        CM_Locate_DevNodeW(&node, instance.pwszVal, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS;
    PropVariantClear(&instance);
    if (!located) return false;
    ULONG bytes = 0, type = 0;
    if (CM_Get_DevNode_Registry_PropertyW(node, CM_DRP_HARDWAREID, &type, nullptr, &bytes, 0) != CR_BUFFER_SMALL ||
        bytes < 2 * sizeof(wchar_t) || bytes > 65536 || bytes % sizeof(wchar_t)) return false;
    std::vector<wchar_t> hardware(bytes / sizeof(wchar_t));
    if (CM_Get_DevNode_Registry_PropertyW(node, CM_DRP_HARDWAREID, &type, hardware.data(), &bytes, 0) != CR_SUCCESS || type != REG_MULTI_SZ ||
        bytes > hardware.size() * sizeof(wchar_t) || bytes % sizeof(wchar_t) || !hasFxVadHardwareId(hardware.data(), bytes / sizeof(wchar_t))) return false;
    wchar_t service[256]{}; bytes = sizeof(service);
    return CM_Get_DevNode_Registry_PropertyW(node, CM_DRP_SERVICE, &type, service, &bytes, 0) == CR_SUCCESS && type == REG_SZ &&
        bytes >= sizeof(wchar_t) && bytes <= sizeof(service) && bytes % sizeof(wchar_t) == 0 && service[bytes / sizeof(wchar_t) - 1] == 0 &&
        _wcsicmp(service, L"fxvad") == 0;
}
std::vector<Endpoint> enumerateEndpoints() {
    auto e = enumerator(); ComPtr<IMMDeviceCollection> devices;
    if (FAILED(e->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) throw std::runtime_error("Audio enumeration failed");
    UINT count = 0; devices->GetCount(&count);
    const auto console = defaultEndpoint(eConsole), multimedia = defaultEndpoint(eMultimedia);
    std::vector<Endpoint> result;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> d; if (FAILED(devices->Item(i, &d))) continue;
        LPWSTR id = nullptr; if (FAILED(d->GetId(&id))) continue;
        Endpoint item; item.id = id; CoTaskMemFree(id);
        item.consoleDefault = item.id == console; item.multimediaDefault = item.id == multimedia;
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT v; PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) item.name = utf8(v.pwszVal);
            PropVariantClear(&v);
        }
        item.virtualDevice = isFxVadEndpoint(d.Get(), e.Get());
        ComPtr<IAudioClient> client;
        if (SUCCEEDED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf())))) {
            WAVEFORMATEX* f = nullptr;
            if (SUCCEEDED(client->GetMixFormat(&f)) && f) { item.channels = f->nChannels; item.sampleRate = f->nSamplesPerSec; item.formatSupported = validateAudioFormat(f); CoTaskMemFree(f); }
        }
        ComPtr<IAudioEndpointVolume> volume;
        if (SUCCEEDED(d->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(volume.GetAddressOf())))) {
            BOOL muted = FALSE;
            item.volumeKnown = SUCCEEDED(volume->GetMasterVolumeLevelScalar(&item.volume)) && SUCCEEDED(volume->GetMute(&muted));
            item.muted = muted != FALSE;
        }
        result.push_back(item);
    }
    return result;
}
bool setDefaultEndpoint(const std::wstring& id, unsigned role) {
    if (id.empty() || role > eCommunications) return false;
    if (defaultEndpoint(role) == id) return true;
    ComPtr<IPolicyConfigVista> policy;
    if (FAILED(CoCreateInstance(__uuidof(CPolicyConfigVistaClient), nullptr, CLSCTX_ALL, __uuidof(IPolicyConfigVista), reinterpret_cast<void**>(policy.GetAddressOf())))) return false;
    const bool success = SUCCEEDED(policy->SetDefaultEndpoint(id.c_str(), static_cast<ERole>(role)));
    if (success) hikariOnOwnDefaultWrite(role, id.c_str());
    return success;
}
bool restoreVolume(const std::wstring& id, float value, bool muted) {
    if (id.empty()) return false;
    auto e = enumerator(); ComPtr<IMMDevice> d;
    if (FAILED(e->GetDevice(id.c_str(), &d))) return false;
    ComPtr<IAudioEndpointVolume> volume;
    if (FAILED(d->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(volume.GetAddressOf())))) return false;
    return SUCCEEDED(volume->SetMasterVolumeLevelScalar(std::clamp(value, 0.0f, 1.0f), nullptr)) && SUCCEEDED(volume->SetMute(muted, nullptr));
}
long probeSharedOutputInitialize(const std::wstring& endpointId) noexcept {
    if (endpointId.empty()) return E_INVALIDARG;
    ComPtr<IMMDeviceEnumerator> devices;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices));
    if (FAILED(hr)) return hr;
    ComPtr<IMMDevice> device;
    hr = devices->GetDevice(endpointId.c_str(), &device);
    if (FAILED(hr)) return hr;
    ComPtr<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(hr)) return hr;
    WAVEFORMATEX* mix = nullptr;
    hr = client->GetMixFormat(&mix);
    if (FAILED(hr)) return hr;
    if (!mix) return E_POINTER;
    // Shared-mode Initialize only reports whether the endpoint can be opened; the stream is never started.
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_SESSIONFLAGS_DISPLAY_HIDE, 200000, 0, mix, nullptr);
    CoTaskMemFree(mix);
    return hr;
}
bool officialFxSoundRunning() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) throw std::runtime_error("Process enumeration failed");
    PROCESSENTRY32W p{}; p.dwSize = sizeof(p); bool found = false;
    if (Process32FirstW(snapshot, &p)) do { if (_wcsicmp(p.szExeFile, L"FxSound.exe") == 0) { found = true; break; } } while (Process32NextW(snapshot, &p));
    CloseHandle(snapshot); return found;
}
}

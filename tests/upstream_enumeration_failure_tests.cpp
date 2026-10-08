// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Hikari
#include <windows.h>
#include <objbase.h>
#include "codedefs.h"
#include "sndDevices.h"
#include "u_sndDevices.h"
#include "pstr.h"
#include "startup_diagnostics.h"
#include <cwchar>
#include <memory>
#include <stdexcept>
#include <vector>
#include <propvarutil.h>

namespace {
unsigned enumerationCalls = 0, propertyCalls = 0;
DWORD enumerationMask = 0;
bool activePropertyFailure = false, thirdEndpointActive = false;
unsigned injectedFailure = 0;
constexpr const wchar_t* endpointIds[] = {L"fixture-virtual", L"fixture-active", L"fixture-hotplug"};
class FixtureProperties final : public IPropertyStore {
public:
    explicit FixtureProperties(unsigned index) : index_(index) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(IPropertyStore)) return E_NOINTERFACE;
        *value = static_cast<IPropertyStore*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { auto remaining = --references_; if (!remaining) delete this; return remaining; }
    HRESULT STDMETHODCALLTYPE GetCount(DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetAt(DWORD, PROPERTYKEY*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetValue(REFPROPERTYKEY key, PROPVARIANT* value) override {
        if (!value) return E_POINTER;
        if (IsEqualPropertyKey(key, PKEY_AudioEndpoint_FormFactor)) { value->vt = VT_UI4; value->uintVal = Speakers; return S_OK; }
        if (IsEqualPropertyKey(key, PKEY_Device_FriendlyName) || IsEqualPropertyKey(key, PKEY_Device_DeviceDesc))
            return InitPropVariantFromString(index_ == 0 ? SND_DEVICES_DFX_DEVICE_STRING : L"fixture physical output", value);
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetValue(REFPROPERTYKEY, REFPROPVARIANT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Commit() override { return E_NOTIMPL; }
private:
    unsigned index_;
    ULONG references_ = 1;
};
class FixtureDevice final : public IMMDevice {
public:
    explicit FixtureDevice(unsigned index, bool defaultEndpoint = false) : index_(index), defaultEndpoint_(defaultEndpoint) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(IMMDevice)) return E_NOINTERFACE;
        *value = static_cast<IMMDevice*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { auto remaining = --references_; if (!remaining) delete this; return remaining; }
    HRESULT STDMETHODCALLTYPE Activate(REFIID, DWORD, PROPVARIANT*, void**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD, IPropertyStore** properties) override {
        ++propertyCalls; if (!properties) return E_POINTER; *properties = nullptr;
        if ((index_ == 2 && !thirdEndpointActive) || (index_ == 1 && (activePropertyFailure || injectedFailure == 7))) return E_FAIL;
        *properties = new FixtureProperties(index_); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetId(LPWSTR* value) override {
        if (!value) return E_POINTER;
        if ((injectedFailure == 5 && defaultEndpoint_) || (injectedFailure == 11 && !defaultEndpoint_)) { *value = nullptr; return E_FAIL; }
        const auto id = endpointIds[index_];
        const auto length = std::wcslen(id) + 1;
        *value = static_cast<LPWSTR>(CoTaskMemAlloc(length * sizeof(*id)));
        if (!*value) return E_OUTOFMEMORY;
        std::wmemcpy(*value, id, length); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetState(DWORD* value) override { if (!value) return E_POINTER; if (injectedFailure == 9) return E_FAIL; *value = index_ == 2 && !thirdEndpointActive ? DEVICE_STATE_UNPLUGGED : DEVICE_STATE_ACTIVE; return S_OK; }
private:
    unsigned index_;
    bool defaultEndpoint_;
    ULONG references_ = 1;
};
class FixtureCollection final : public IMMDeviceCollection {
public:
    explicit FixtureCollection(DWORD mask) {
        for (unsigned index = 0; index < 3; ++index)
            if (mask & (index == 2 && !thirdEndpointActive ? DEVICE_STATE_UNPLUGGED : DEVICE_STATE_ACTIVE)) indices_.push_back(index);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(IMMDeviceCollection)) return E_NOINTERFACE;
        *value = static_cast<IMMDeviceCollection*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { auto remaining = --references_; if (!remaining) delete this; return remaining; }
    HRESULT STDMETHODCALLTYPE GetCount(UINT* value) override { if (!value) return E_POINTER; if (injectedFailure == 3) return E_FAIL; *value = static_cast<UINT>(indices_.size()); return S_OK; }
    HRESULT STDMETHODCALLTYPE Item(UINT index, IMMDevice** value) override {
        if (!value) return E_POINTER;
        *value = nullptr; if (injectedFailure == 6) return E_FAIL; if (index >= indices_.size()) return E_INVALIDARG;
        *value = new FixtureDevice(indices_[index]); return S_OK;
    }
private:
    ULONG references_ = 1;
    std::vector<unsigned> indices_;
};
class FixtureEnumerator final : public IMMDeviceEnumerator {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(IMMDeviceEnumerator)) return E_NOINTERFACE;
        *value = static_cast<IMMDeviceEnumerator*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { auto remaining = --references_; if (!remaining) delete this; return remaining; }
    HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(EDataFlow flow, DWORD mask, IMMDeviceCollection** value) override {
        ++enumerationCalls; enumerationMask = mask;
        if (!value) return E_POINTER;
        *value = nullptr; if (injectedFailure == 2) return E_FAIL; if (flow != eRender) return E_INVALIDARG;
        *value = new FixtureCollection(mask); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(EDataFlow, ERole, IMMDevice** value) override {
        if (!value) return E_POINTER; *value = nullptr; if (injectedFailure == 4) return E_FAIL; *value = new FixtureDevice(1, true); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR, IMMDevice**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IMMNotificationClient*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IMMNotificationClient*) override { return E_NOTIMPL; }
private:
    ULONG references_ = 1;
};
HRESULT WINAPI fixtureCoCreateInstance(REFCLSID type, LPUNKNOWN, DWORD, REFIID id, LPVOID* value) {
    if (!value) return E_POINTER;
    *value = nullptr;
    if (injectedFailure == 1) return E_FAIL;
    if (type != __uuidof(MMDeviceEnumerator) || id != __uuidof(IMMDeviceEnumerator)) return E_NOINTERFACE;
    *value = static_cast<IMMDeviceEnumerator*>(new FixtureEnumerator()); return S_OK;
}
int fixtureGetFormatFromID(PT_HANDLE*, wchar_t*, WAVEFORMATEX* format, int* status) {
    if (injectedFailure == 8) return -1;
    *format = {}; format->nChannels = 2; format->nSamplesPerSec = 48000;
    *status = SND_DEVICES_DEVICE_OPERATION_COMPLETED; return OKAY;
}
int fixtureClassify(wchar_t* text, wchar_t* needle, int start, int* position, int* found) {
    if (injectedFailure == 10) return -1;
    return pstrCalcLocationOfStrInStr_Wide(text, needle, start, position, found);
}
}

// Compile the actual pinned upstream function with only its COM creation replaced.
#define CoCreateInstance fixtureCoCreateInstance
#define sndDevices_GetAll fixtureSndDevicesGetAll
#define sndDevicesGetFormatFromID fixtureGetFormatFromID
#define pstrCalcLocationOfStrInStr_Wide fixtureClassify
#include "sndDevices_GetAll.cpp"
#undef pstrCalcLocationOfStrInStr_Wide
#undef sndDevicesGetFormatFromID
#undef sndDevices_GetAll
#undef CoCreateInstance

namespace hikari {
void runUpstreamEnumerationFailureTests() {
    enumerationCalls = propertyCalls = 0; enumerationMask = 0;
    auto state = std::make_unique<sndDevicesHdlType>();
    state->CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
    state->IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
    state->numRealDevices = state->numPreviousRealDevices = 1;
    int count = 0;
    const int result = fixtureSndDevicesGetAll(reinterpret_cast<PT_HANDLE*>(state.get()), &count);
    struct ReleaseDevices {
        sndDevicesHdlType* state;
        ~ReleaseDevices() { for (auto& device : state->pAllDevices) if (device) device->Release(); }
    } release{state.get()};
    if (result != OKAY || enumerationCalls != 1 || propertyCalls != 2 || count != 2 ||
        enumerationMask != DEVICE_STATE_ACTIVE || state->numRealDevices != 1 || state->dfxDeviceNum != 0 ||
        std::wcscmp(state->pwszIDRealDevices[0], endpointIds[1]) != 0)
        throw std::runtime_error("Unplugged property failure poisoned active enumeration");
    activePropertyFailure = true;
    std::vector<std::string> failures;
    StartupDiagnostics diagnostics([&](const std::string& line) { failures.push_back(line); });
    StartupEnumerationScope scope(diagnostics);
    state->function_status = SND_DEVICES_DEVICE_OPERATION_COMPLETED;
    const int failed = fixtureSndDevicesGetAll(reinterpret_cast<PT_HANDLE*>(state.get()), &count);
    if (failed != OKAY || state->function_status != SND_DEVICES_INSTANCE_CREATE_FAILED || state->numRealDevices != 0)
        throw std::runtime_error("Active property failure fabricated a successful output list");
    try { diagnostics.run([&] { diagnostics.step(StartupStep::outputSelection); throw std::runtime_error("private endpoint identity"); }); }
    catch (const std::runtime_error&) {}
    if (failures.size() != 1 || failures.front() != "Startup failure enumeration ENDPOINT_PROPERTIES_FAILED 0x80004005")
        throw std::runtime_error("GetAll property failure diagnostic missing");
    activePropertyFailure = false; thirdEndpointActive = true;
    state->function_status = SND_DEVICES_DEVICE_OPERATION_COMPLETED;
    if (fixtureSndDevicesGetAll(reinterpret_cast<PT_HANDLE*>(state.get()), &count) != OKAY || count != 3 || state->numRealDevices != 2)
        throw std::runtime_error("Connected active endpoint was not re-enumerated");
    thirdEndpointActive = false;
    if (fixtureSndDevicesGetAll(reinterpret_cast<PT_HANDLE*>(state.get()), &count) != OKAY || count != 2 || state->numRealDevices != 1)
        throw std::runtime_error("Disconnected endpoint remained in the active output list");
    constexpr const char* codes[] = {"ENUMERATOR_CREATE_FAILED", "ENDPOINT_ENUMERATION_FAILED", "ENDPOINT_COUNT_FAILED",
        "DEFAULT_ENDPOINT_FAILED", "ENDPOINT_ID_FAILED", "ENDPOINT_ITEM_FAILED", "ENDPOINT_PROPERTIES_FAILED",
        "ENDPOINT_FORMAT_FAILED", "ENDPOINT_STATE_FAILED", "ENDPOINT_CLASSIFICATION_FAILED", "ENDPOINT_ID_FAILED"};
    for (injectedFailure = 1; injectedFailure <= 11; ++injectedFailure) {
        failures.clear(); state->function_status = SND_DEVICES_DEVICE_OPERATION_COMPLETED;
        fixtureSndDevicesGetAll(reinterpret_cast<PT_HANDLE*>(state.get()), &count);
        if (state->numRealDevices != 0) throw std::runtime_error("Failed enumeration retained an active output list");
        try { diagnostics.run([&] { diagnostics.step(StartupStep::outputSelection); throw std::runtime_error("private endpoint identity"); }); }
        catch (const std::runtime_error&) {}
        const auto expected = std::string("Startup failure enumeration ") + codes[injectedFailure - 1] +
            (injectedFailure == 8 || injectedFailure == 10 ? " 0x00000000" : " 0x80004005");
        if (failures.size() != 1 || failures.front() != expected)
            throw std::runtime_error("Real GetAll failure did not retain the corresponding stage and HRESULT");
    }
    injectedFailure = 0;
}
}

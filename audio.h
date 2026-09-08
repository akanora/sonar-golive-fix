#pragma once
// ============================================================================
//  audio.h — Audio device enumeration and per-device app muting (header-only)
// ============================================================================

#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <propidl.h>
#include <string>
#include <vector>

// PKEY_Device_FriendlyName defined manually to sidestep header-order problems.
// GUID {a45c254e-df1c-4efd-8020-67d146a850e0}, PID 14
static const PROPERTYKEY kPKEY_Device_FriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd,
      { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } },
    14
};

// ---- Data types -----------------------------------------------------------

struct DeviceInfo {
    std::wstring id;    // Endpoint device ID (opaque string)
    std::wstring name;  // Human-readable friendly name
};

// ---- Device enumeration ---------------------------------------------------

inline std::vector<DeviceInfo> EnumerateAudioDevices()
{
    std::vector<DeviceInfo> devices;

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) return devices;

    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) {
        enumerator->Release();
        return devices;
    }

    UINT count = 0;
    collection->GetCount(&count);

    for (UINT i = 0; i < count; ++i) {
        IMMDevice* device = nullptr;
        if (FAILED(collection->Item(i, &device))) continue;

        LPWSTR id = nullptr;
        if (FAILED(device->GetId(&id))) {
            device->Release();
            continue;
        }

        IPropertyStore* props = nullptr;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT varName;
            PropVariantInit(&varName);
            if (SUCCEEDED(props->GetValue(kPKEY_Device_FriendlyName, &varName))
                && varName.vt == VT_LPWSTR && varName.pwszVal)
            {
                DeviceInfo info;
                info.id   = id;
                info.name = varName.pwszVal;
                devices.push_back(std::move(info));
            }
            PropVariantClear(&varName);
            props->Release();
        }

        CoTaskMemFree(id);
        device->Release();
    }

    collection->Release();
    enumerator->Release();
    return devices;
}

// ---- Per-device app muting ------------------------------------------------
// Returns true if the target app was found on the device (already muted or
// freshly muted).  Returns false if the app has no audio session on the device.

inline bool MuteAppOnDevice(const std::wstring& deviceId,
                            const std::wstring& targetApp)
{
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) return false;

    IMMDevice* device = nullptr;
    hr = enumerator->GetDevice(deviceId.c_str(), &device);
    if (FAILED(hr)) {
        enumerator->Release();
        return false;
    }

    IAudioSessionManager2* mgr = nullptr;
    hr = device->Activate(
        __uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(&mgr));
    if (FAILED(hr)) {
        device->Release();
        enumerator->Release();
        return false;
    }

    IAudioSessionEnumerator* sessions = nullptr;
    hr = mgr->GetSessionEnumerator(&sessions);
    if (FAILED(hr)) {
        mgr->Release();
        device->Release();
        enumerator->Release();
        return false;
    }

    int sessionCount = 0;
    sessions->GetCount(&sessionCount);
    bool found = false;

    for (int i = 0; i < sessionCount; ++i) {
        IAudioSessionControl* ctrl = nullptr;
        if (FAILED(sessions->GetSession(i, &ctrl))) continue;

        IAudioSessionControl2* ctrl2 = nullptr;
        if (SUCCEEDED(ctrl->QueryInterface(
                __uuidof(IAudioSessionControl2),
                reinterpret_cast<void**>(&ctrl2))))
        {
            DWORD pid = 0;
            ctrl2->GetProcessId(&pid);

            if (pid != 0) {
                HANDLE proc = OpenProcess(
                    PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (proc) {
                    WCHAR fullPath[MAX_PATH] = {};
                    DWORD pathLen = MAX_PATH;
                    if (QueryFullProcessImageNameW(proc, 0, fullPath, &pathLen))
                    {
                        // Extract filename from full path
                        const WCHAR* filename = wcsrchr(fullPath, L'\\');
                        filename = filename ? filename + 1 : fullPath;

                        if (_wcsicmp(filename, targetApp.c_str()) == 0) {
                            ISimpleAudioVolume* vol = nullptr;
                            if (SUCCEEDED(ctrl->QueryInterface(
                                    __uuidof(ISimpleAudioVolume),
                                    reinterpret_cast<void**>(&vol))))
                            {
                                BOOL isMuted = FALSE;
                                vol->GetMute(&isMuted);
                                if (!isMuted) {
                                    vol->SetMute(TRUE, nullptr);
                                }
                                found = true;
                                vol->Release();
                            }
                        }
                    }
                    CloseHandle(proc);
                }
            }
            ctrl2->Release();
        }
        ctrl->Release();
    }

    sessions->Release();
    mgr->Release();
    device->Release();
    enumerator->Release();
    return found;
}

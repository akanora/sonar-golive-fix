#include <windows.h>
#include <mmdeviceapi.h>
#include <thread>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <iostream>
#include <string>
#include <comdef.h>
#include <psapi.h>
#include <functiondiscoverykeys_devpkey.h> // Include for PKEY_Device_FriendlyName
#include <initguid.h>
#include <devpkey.h>
#include <propkeydef.h> // Only if available

DEFINE_PROPERTYKEY(PKEY_Device_FriendlyName, 
    0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0, 14);

// Function to convert wide string to string
std::string WideStringToString(const std::wstring& wideStr) {
    int bufferSize = WideCharToMultiByte(CP_UTF8, 0, wideStr.c_str(), -1, NULL, 0, NULL, NULL);
    std::string str(bufferSize, 0);
    WideCharToMultiByte(CP_UTF8, 0, wideStr.c_str(), -1, &str[0], bufferSize, NULL, NULL);
    return str;
}

bool MuteDiscordOnSpecificDevices(const std::wstring& deviceName, const std::wstring& targetApp) {
    CoInitialize(nullptr);

    IMMDeviceEnumerator* deviceEnumerator = nullptr;
    IMMDevice* targetDevice = nullptr;

    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&deviceEnumerator)
    );
    if (FAILED(hr)) {
        std::cerr << "Failed to create IMMDeviceEnumerator: " << std::hex << hr << "\n";
        return false;
    }

    IMMDeviceCollection* deviceCollection = nullptr;
    hr = deviceEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &deviceCollection);
    if (FAILED(hr)) {
        std::cerr << "Failed to enumerate audio endpoints: " << std::hex << hr << "\n";
        deviceEnumerator->Release();
        return false;
    }

    UINT deviceCount;
    deviceCollection->GetCount(&deviceCount);

    for (UINT i = 0; i < deviceCount; ++i) {
        IMMDevice* device = nullptr;
        hr = deviceCollection->Item(i, &device);
        if (FAILED(hr)) continue;

        LPWSTR deviceId;
        hr = device->GetId(&deviceId);
        if (FAILED(hr)) {
            device->Release();
            continue;
        }

        IPropertyStore* propertyStore = nullptr;
        hr = device->OpenPropertyStore(STGM_READ, &propertyStore);
        if (FAILED(hr)) {
            CoTaskMemFree(deviceId);
            device->Release();
            continue;
        }

        PROPVARIANT friendlyName;
        PropVariantInit(&friendlyName);
        hr = propertyStore->GetValue(PKEY_Device_FriendlyName, &friendlyName); // Fix here
        if (SUCCEEDED(hr)) {
            if (std::wstring(friendlyName.pwszVal).find(deviceName) != std::wstring::npos) {
                targetDevice = device;
                PropVariantClear(&friendlyName);
                propertyStore->Release();
                CoTaskMemFree(deviceId);
                break;
            }
        }

        PropVariantClear(&friendlyName);
        propertyStore->Release();
        CoTaskMemFree(deviceId);
        device->Release();
    }

    if (!targetDevice) {
        std::wcerr << L"Audio device '" << deviceName << L"' not found.\n";
        deviceCollection->Release();
        deviceEnumerator->Release();
        return false;
    }

    IAudioSessionManager2* sessionManager = nullptr;
    hr = targetDevice->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&sessionManager));
    if (FAILED(hr)) {
        std::cerr << "Failed to activate IAudioSessionManager2: " << std::hex << hr << "\n";
        targetDevice->Release();
        deviceCollection->Release();
        deviceEnumerator->Release();
        return false;
    }

    IAudioSessionEnumerator* sessionEnumerator = nullptr;
    hr = sessionManager->GetSessionEnumerator(&sessionEnumerator);
    if (FAILED(hr)) {
        std::cerr << "Failed to get session enumerator: " << std::hex << hr << "\n";
        sessionManager->Release();
        targetDevice->Release();
        deviceCollection->Release();
        deviceEnumerator->Release();
        return false;
    }

    int sessionCount;
    hr = sessionEnumerator->GetCount(&sessionCount);
    if (FAILED(hr)) {
        std::cerr << "Failed to get session count: " << std::hex << hr << "\n";
        sessionEnumerator->Release();
        sessionManager->Release();
        targetDevice->Release();
        deviceCollection->Release();
        deviceEnumerator->Release();
        return false;
    }

    bool muted = false;
    for (int i = 0; i < sessionCount; ++i) {
        IAudioSessionControl* sessionControl = nullptr;
        hr = sessionEnumerator->GetSession(i, &sessionControl);
        if (FAILED(hr)) continue;

        IAudioSessionControl2* sessionControl2 = nullptr;
        hr = sessionControl->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&sessionControl2));
        if (SUCCEEDED(hr)) {
            DWORD processId;
            sessionControl2->GetProcessId(&processId);

            HANDLE processHandle = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, processId);
            if (processHandle) {
                WCHAR processName[MAX_PATH] = {};
                if (GetModuleBaseNameW(processHandle, nullptr, processName, MAX_PATH)) { // Fix here
                    std::wstring processNameStr(processName);
                    if (processNameStr == targetApp) {
                        ISimpleAudioVolume* audioVolume = nullptr;
                        hr = sessionControl->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&audioVolume));
                        if (SUCCEEDED(hr)) {
                            audioVolume->SetMute(TRUE, nullptr);
                            std::wcout << L"Muted volume for Discord on: " << deviceName << "\n";
                            audioVolume->Release();
                            muted = true;
                        }
                    }
                }
                CloseHandle(processHandle);
            }
            sessionControl2->Release();
        }
        sessionControl->Release();
    }

    if (!muted) {
        std::wcerr << L"Discord session not found on device '" << deviceName << L"'.\n";
    }

    sessionEnumerator->Release();
    sessionManager->Release();
    targetDevice->Release();
    deviceCollection->Release();
    deviceEnumerator->Release();
    return muted;
}

void MonitorDiscordMuteStatus(const std::wstring& deviceName, const std::wstring& targetApp) {
    while (true) {
        MuteDiscordOnSpecificDevices(deviceName, targetApp);
        Sleep(5000); // Check every 5 seconds
    }
}

int main() {
    std::wstring device1 = L"Speakers (HyperX Cloud Core Wireless)";
    std::wstring device2 = L"SteelSeries Sonar - Microphone (SteelSeries Sonar Virtual Audio Device)";
    std::wstring targetApp = L"Discord.exe";

    // Start monitoring Discord mute status in the background
    std::thread monitorThread1(MonitorDiscordMuteStatus, device1, targetApp);
    std::thread monitorThread2(MonitorDiscordMuteStatus, device2, targetApp);

    // Wait for user input to exit
    std::cout << "Press Enter to stop the program...\n";
    std::cin.get();

    // Wait for background threads to finish (in case you need to clean up later)
    monitorThread1.join();
    monitorThread2.join();

    return 0;
}
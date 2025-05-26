# Sonar Discord Golive Fix

This tool automatically mutes Discord on specific audio devices.  
Useful for setups using **SteelSeries Sonar** or similar, to prevent others from hearing themselves during a discord stream.

---

## 🔧 Build

To compile the project (requires `g++`, e.g. from MinGW-w64 or MSYS2):

```bash
g++ -static -std=c++17 -o MuteDiscordOnSpecificDevices.exe MuteDiscordOnSpecificDevices.cpp -lole32 -lOleAut32 -lcomctl32 -lpsapi -luuid -Wl,--subsystem,windows
```

## ⚙️ Usage

Edit this line in the code to match your own headset name:

```cpp
std::wstring device1 = L"Speakers (Your Headset Name Here)"; // you can check the full name from sndvol
```

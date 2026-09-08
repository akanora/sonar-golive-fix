# Sonar Discord Golive Fix (Discord Device Muter)

An automated Windows system tray tool that automatically mutes Discord (or any specified application) on specific audio render devices.

Useful for setups using **SteelSeries Sonar**, virtual audio cables, or dual audio output setups to prevent stream echoes or loopback where others hear themselves during a Discord Go Live stream.

---

## ✨ Features

- **No Hardcoded Devices**: Displays a list of all active audio output endpoints with checkboxes. Select the exact devices you want to mute.
- **Starts Minimized**: Runs in the background and sits quietly in your system tray (notification area).
- **Custom Mute Icon**: Displays a dedicated speaker mute icon in the system tray and title bar.
- **Auto-Persistent Settings**: Your checked devices, target process name, and startup preferences are saved to `DiscordMuter.ini` and restored between launches.
- **Responsive Refresh**: Adding or removing audio devices won't lose your checked selections or freeze the UI.
- **Configurable Target**: Defaults to `Discord.exe`, but you can change it to mute any Windows app or game.
- **Optional Auto-Start**: Includes a "Start with Windows" checkbox using registry auto-run.
- **100% Portable**: Statically compiled binary — no MinGW, runtime installers, or extra DLLs needed on other PCs.

---

## 🔧 Build Instructions (MinGW-w64)

The project is designed to be compiled with **MinGW-w64** (`g++`) and statically linked.

### Prerequisites

You need MinGW-w64 / MSYS2 with `g++`. If you don't have it yet, you can install it via [MSYS2](https://www.msys2.org/):

```bash
pacman -S --needed base-devel mingw-w64-ucrt-x86_64-toolchain
```

Make sure `C:\msys64\ucrt64\bin` (or your MinGW `bin` folder) is in your `PATH`.

### Method 1: Using `build.bat` (Recommended)

Simply double-click or run `build.bat`. It will automatically search for MinGW in your `PATH` and common MSYS2 locations (`C:\msys64\ucrt64`, `C:\msys64\mingw64`, etc.), compile the application with static linking, and output `DiscordMuter.exe`.

### Method 2: Command Line Compilation

Run the following command in terminal:

```bash
g++ -std=c++17 -O2 -Wall -Wno-unknown-pragmas -Wno-unused-function -DUNICODE -D_UNICODE -static -mwindows main.cpp -o DiscordMuter.exe -lole32 -lcomctl32 -lshell32 -lgdi32 -luuid
```

> **Why `-static`?**  
> Linking statically ensures that the generated `DiscordMuter.exe` contains all necessary runtime routines and dependencies. You can distribute this single `.exe` file to any Windows machine and it will work immediately without requiring MSYS2, MinGW, or runtime DLLs.

---

## ⚙️ Usage

1. Launch `DiscordMuter.exe`.
   - By default, it will start minimized in your system tray (bottom-right near your clock).
2. **Left-click** the system tray mute icon to open the configuration window.
3. Check the audio devices where Discord should be muted (e.g., your virtual stream audio device or secondary headset channel).
4. You can also configure:
   - **Target Application**: Change `Discord.exe` if desired.
   - **Start with Windows**: Automatically start this tool on boot.
   - **Start minimized to tray**: Launch directly into the system tray.
5. Close the window (clicking the `X` hides it back to the tray).
6. To completely close the application, **right-click** the tray icon and choose **Exit**.

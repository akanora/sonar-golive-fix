@echo off
setlocal enabledelayedexpansion

:: ============================================================================
::  build.bat — Build DiscordMuter.exe with MinGW-w64 (GCC)
::
::  Produces a standalone, portable Windows executable with static linking.
::  The resulting EXE runs on any Windows PC without needing MinGW or DLLs.
:: ============================================================================

set "GPP="

:: 1. Check if g++ is already in system PATH
where g++ >nul 2>&1 && set "GPP=g++"

:: 2. Check common MSYS2 default installation locations
if not defined GPP if exist "C:\msys64\ucrt64\bin\g++.exe" (
    set "PATH=C:\msys64\ucrt64\bin;!PATH!"
    set "GPP=C:\msys64\ucrt64\bin\g++.exe"
)
if not defined GPP if exist "C:\msys64\mingw64\bin\g++.exe" (
    set "PATH=C:\msys64\mingw64\bin;!PATH!"
    set "GPP=C:\msys64\mingw64\bin\g++.exe"
)

:: 3. Check standalone MinGW locations
if not defined GPP if exist "C:\mingw64\bin\g++.exe" (
    set "PATH=C:\mingw64\bin;!PATH!"
    set "GPP=C:\mingw64\bin\g++.exe"
)

if not defined GPP (
    echo.
    echo  ERROR: MinGW g++ compiler not found!
    echo.
    echo  Please ensure MinGW-w64 / MSYS2 is installed and g++ is in your PATH.
    echo  If using MSYS2, you can install the UCRT64 toolchain using:
    echo      pacman -S --needed base-devel mingw-w64-ucrt-x86_64-toolchain
    echo.
    pause
    exit /b 1
)

echo [*] Compiling DiscordMuter.exe with MinGW g++...
"%GPP%" -std=c++17 -O2 -Wall -Wno-unknown-pragmas -Wno-unused-function -DUNICODE -D_UNICODE ^
    -static -mwindows main.cpp -o DiscordMuter.exe ^
    -lole32 -lcomctl32 -lshell32 -lgdi32 -luuid

if !ERRORLEVEL! NEQ 0 (
    echo.
    echo  BUILD FAILED! Check the errors above.
    echo.
    pause
    exit /b 1
)

echo.
echo  ==============================================================
echo  BUILD SUCCESSFUL: DiscordMuter.exe
echo  The executable is statically linked and portable.
echo  It can run on any Windows PC without installing MinGW or runtimes.
echo  ==============================================================
echo.
pause
exit /b 0

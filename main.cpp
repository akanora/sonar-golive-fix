// ============================================================================
//  main.cpp — Discord Device Muter  (Win32 system-tray application)
//
//  Starts minimized to the system tray.  Left-click the tray icon to open the
//  settings window, pick audio devices with checkboxes, and the app will keep
//  the target process muted on those devices in the background.
// ============================================================================

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <mutex>

#include "audio.h"

// ---- Linker directives ----------------------------------------------------
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

// Common Controls v6 (visual-styles) manifest via pragma
#pragma comment(linker, "/manifestdependency:\"type='win32' "                \
    "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' "           \
    "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' "          \
    "language='*'\"")

// ---- Constants & IDs ------------------------------------------------------

static const wchar_t* const kClassName = L"DiscordMuterClass";
static const wchar_t* const kAppTitle  = L"Discord Device Muter";
static const wchar_t* const kRegRunKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* const kRegValueName = L"DiscordDeviceMuter";

static const UINT WM_TRAYICON      = WM_USER + 1;
static const UINT WM_UPDATE_STATUS = WM_USER + 2;

enum ControlId : WORD {
    IDC_LISTVIEW      = 1001,
    IDC_EDIT_TARGET   = 1002,
    IDC_BTN_REFRESH   = 1003,
    IDC_LABEL_STATUS  = 1004,
    IDC_LABEL_TARGET  = 1005,
    IDC_CHK_AUTOSTART = 1006,
    IDC_CHK_MINIMIZED = 1007,
    ID_TRAY_SHOW      = 3001,
    ID_TRAY_EXIT      = 3002,
};

// ---- Globals --------------------------------------------------------------

static HINSTANCE       g_hInst         = nullptr;
static HWND            g_hWnd          = nullptr;
static HWND            g_hListView     = nullptr;
static HWND            g_hEditTarget   = nullptr;
static HWND            g_hLabelStatus  = nullptr;
static HWND            g_hBtnRefresh   = nullptr;
static HWND            g_hLabelTarget  = nullptr;
static HWND            g_hChkAutoStart = nullptr;
static HWND            g_hChkMinimized = nullptr;
static NOTIFYICONDATAW g_nid           = {};
static HANDLE          g_hThread       = nullptr;
static HANDLE          g_hStopEvent    = nullptr;
static HFONT           g_hFont         = nullptr;
static HICON           g_hIconSmall    = nullptr;  // 16×16 mute icon
static HICON           g_hIconLarge    = nullptr;  // 32×32 mute icon

// Shared state between UI thread and monitor thread
static std::mutex                g_mutex;
static std::vector<std::wstring> g_selectedDeviceIds;
static std::wstring              g_targetApp = L"Discord.exe";

// Device cache (UI thread only — no lock needed)
static std::vector<DeviceInfo>   g_devices;

// Flag: suppress LVN_ITEMCHANGED processing during bulk ListView operations
// (inserting items, restoring checkmarks) to avoid O(N²) cascading updates.
static bool g_bSuppressNotify = false;

// ---- Forward declarations -------------------------------------------------

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
DWORD   WINAPI   MonitorThreadProc(LPVOID);
static void CreateControls(HWND hWnd);
static void LayoutControls(HWND hWnd);
static void PopulateDeviceList();
static void UpdateSelectedDevices();
static void AddTrayIcon(HWND hWnd);
static void RemoveTrayIcon();
static void ShowTrayMenu(HWND hWnd);
static std::wstring GetSettingsPath();
static void SaveSettings();
static void LoadSettings();
static void SetAutoStart(bool enable);
static bool GetAutoStart();
static HICON CreateMuteIcon(int size);

// ===========================================================================
//  Entry point
// ===========================================================================

static int AppMain(HINSTANCE hInstance)
{
    // ---- Single-instance check --------------------------------------------
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"DiscordDeviceMuter_Single");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(kClassName, nullptr);
        if (existing) {
            ShowWindow(existing, SW_SHOW);
            SetForegroundWindow(existing);
        }
        return 0;
    }

    g_hInst = hInstance;

    // ---- COM & common controls --------------------------------------------
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icex);

    // ---- Stop event for clean thread shutdown -----------------------------
    g_hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    // ---- Register window class --------------------------------------------
    g_hIconSmall = CreateMuteIcon(16);
    g_hIconLarge = CreateMuteIcon(32);

    WNDCLASSEXW wc  = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInstance;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName  = kClassName;
    wc.hIcon          = g_hIconLarge;
    wc.hIconSm        = g_hIconSmall;
    RegisterClassExW(&wc);

    // ---- Create main window (starts hidden) -------------------------------
    g_hWnd = CreateWindowExW(
        0, kClassName, kAppTitle,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 560, 500,
        nullptr, nullptr, hInstance, nullptr);

    if (!g_hWnd) {
        CoUninitialize();
        return 1;
    }

    // Check "Start minimized" setting — if unchecked, show the window now
    {
        std::wstring ini = GetSettingsPath();
        int startMin = GetPrivateProfileIntW(
            L"Settings", L"StartMinimized", 1, ini.c_str());
        if (!startMin) {
            ShowWindow(g_hWnd, SW_SHOW);
        }
    }

    // ---- Start background monitor thread ----------------------------------
    g_hThread = CreateThread(nullptr, 0, MonitorThreadProc, nullptr, 0, nullptr);

    // ---- Message loop -----------------------------------------------------
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // ---- Cleanup ----------------------------------------------------------
    SetEvent(g_hStopEvent);
    if (g_hThread) {
        WaitForSingleObject(g_hThread, 6000);
        CloseHandle(g_hThread);
    }
    CloseHandle(g_hStopEvent);
    if (g_hFont) DeleteObject(g_hFont);
    if (g_hIconSmall) DestroyIcon(g_hIconSmall);
    if (g_hIconLarge) DestroyIcon(g_hIconLarge);
    if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
    CoUninitialize();

    return static_cast<int>(msg.wParam);
}

// Entry points — MSVC uses wWinMain, MinGW uses WinMain.
// Both forward to AppMain above.
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    return AppMain(hInstance);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int)
{
    return AppMain(hInstance);
}

// ===========================================================================
//  Window procedure
// ===========================================================================

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {

    // ---- Window creation --------------------------------------------------
    case WM_CREATE:
        g_bSuppressNotify = true;
        CreateControls(hWnd);
        AddTrayIcon(hWnd);
        PopulateDeviceList();
        LoadSettings();
        g_bSuppressNotify = false;
        return 0;

    // ---- Resize / minimize ------------------------------------------------
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            ShowWindow(hWnd, SW_HIDE);   // minimise → hide to tray
            return 0;
        }
        LayoutControls(hWnd);
        return 0;

    // ---- Button clicks & edit changes -------------------------------------
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_REFRESH:
            g_bSuppressNotify = true;
            PopulateDeviceList();
            g_bSuppressNotify = false;
            break;
        case IDC_EDIT_TARGET:
            if (HIWORD(wParam) == EN_CHANGE && !g_bSuppressNotify) {
                wchar_t buf[MAX_PATH] = {};
                GetWindowTextW(g_hEditTarget, buf, MAX_PATH);
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_targetApp = buf;
                }
                SaveSettings();
            }
            break;
        case IDC_CHK_AUTOSTART: {
            bool checked = (SendMessageW(g_hChkAutoStart, BM_GETCHECK, 0, 0)
                            == BST_CHECKED);
            SetAutoStart(checked);
            SaveSettings();
            break;
        }
        case IDC_CHK_MINIMIZED:
            SaveSettings();
            break;
        }
        return 0;

    // ---- ListView checkbox changes ----------------------------------------
    case WM_NOTIFY: {
        auto* nmhdr = reinterpret_cast<LPNMHDR>(lParam);
        if (nmhdr->idFrom == IDC_LISTVIEW
            && nmhdr->code == LVN_ITEMCHANGED
            && !g_bSuppressNotify)
        {
            auto* nmlv = reinterpret_cast<LPNMLISTVIEW>(lParam);
            if (nmlv->uChanged & LVIF_STATE) {
                UINT oldChk = (nmlv->uOldState & LVIS_STATEIMAGEMASK) >> 12;
                UINT newChk = (nmlv->uNewState & LVIS_STATEIMAGEMASK) >> 12;
                if (oldChk != newChk) {
                    UpdateSelectedDevices();
                }
            }
        }
        break;
    }

    // ---- System tray callbacks --------------------------------------------
    case WM_TRAYICON:
        switch (LOWORD(lParam)) {
        case WM_LBUTTONUP:
            if (IsWindowVisible(hWnd)) {
                ShowWindow(hWnd, SW_HIDE);
            } else {
                ShowWindow(hWnd, SW_SHOW);
                ShowWindow(hWnd, SW_RESTORE);   // in case it was minimised
                SetForegroundWindow(hWnd);
            }
            break;
        case WM_RBUTTONUP:
            ShowTrayMenu(hWnd);
            break;
        }
        return 0;

    // ---- Status update from monitor thread --------------------------------
    case WM_UPDATE_STATUS: {
        int found   = static_cast<int>(wParam);
        int devices = static_cast<int>(lParam);
        wchar_t status[256] = {};
        if (devices == 0)
            wcscpy_s(status, L"Status: No devices selected");
        else if (found > 0)
            swprintf_s(status, L"Status: Muted on %d device(s)", found);
        else
            swprintf_s(status, L"Status: Monitoring %d device(s)\u2026",
                       devices);
        SetWindowTextW(g_hLabelStatus, status);

        // Update tray tooltip too
        swprintf_s(g_nid.szTip, L"Discord Muter \u2014 %s", status + 8);
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
        return 0;
    }

    // ---- Close hides to tray; Destroy quits -------------------------------
    case WM_CLOSE:
        ShowWindow(hWnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ===========================================================================
//  UI helpers
// ===========================================================================

static void CreateControls(HWND hWnd)
{
    // System font for a native look
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_hFont = CreateFontIndirectW(&ncm.lfMessageFont);

    // "Target Application:" label
    g_hLabelTarget = CreateWindowW(
        L"STATIC", L"Target Application:",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, hWnd,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_LABEL_TARGET)),
        g_hInst, nullptr);

    // Edit box (defaults to Discord.exe)
    g_hEditTarget = CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"Discord.exe",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0, 0, 0, 0, hWnd,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_EDIT_TARGET)),
        g_hInst, nullptr);

    // Device ListView with checkboxes
    g_hListView = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_NOSORTHEADER,
        0, 0, 0, 0, hWnd,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_LISTVIEW)),
        g_hInst, nullptr);

    ListView_SetExtendedListViewStyle(g_hListView,
        LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    // Single column
    LVCOLUMNW lvc = {};
    lvc.mask    = LVCF_WIDTH | LVCF_TEXT;
    lvc.cx      = 500;
    lvc.pszText = const_cast<LPWSTR>(L"Audio Device");
    ListView_InsertColumn(g_hListView, 0, &lvc);

    // Refresh button
    g_hBtnRefresh = CreateWindowW(
        L"BUTTON", L"Refresh Devices",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, hWnd,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_BTN_REFRESH)),
        g_hInst, nullptr);

    // Status label
    g_hLabelStatus = CreateWindowW(
        L"STATIC", L"Status: Waiting\u2026",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, hWnd,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_LABEL_STATUS)),
        g_hInst, nullptr);

    // "Start with Windows" checkbox
    g_hChkAutoStart = CreateWindowW(
        L"BUTTON", L"Start with Windows",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        0, 0, 0, 0, hWnd,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_CHK_AUTOSTART)),
        g_hInst, nullptr);

    // "Start minimized" checkbox
    g_hChkMinimized = CreateWindowW(
        L"BUTTON", L"Start minimized to tray",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        0, 0, 0, 0, hWnd,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(IDC_CHK_MINIMIZED)),
        g_hInst, nullptr);

    // Apply font to all controls
    for (HWND h : { g_hLabelTarget, g_hEditTarget, g_hListView,
                    g_hBtnRefresh, g_hLabelStatus,
                    g_hChkAutoStart, g_hChkMinimized }) {
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_hFont), TRUE);
    }
}

static void LayoutControls(HWND hWnd)
{
    RECT rc;
    GetClientRect(hWnd, &rc);
    const int W      = rc.right;
    const int H      = rc.bottom;
    const int M      = 12;          // margin
    const int ctrlH  = 24;
    const int labelW = 140;
    const int btnW   = 130;
    const int btnH   = 30;
    const int chkH   = 22;

    int y = M;

    // Row 1: label + edit
    MoveWindow(g_hLabelTarget, M, y + 3, labelW, ctrlH, TRUE);
    MoveWindow(g_hEditTarget, M + labelW + 4, y,
               W - M * 2 - labelW - 4, ctrlH, TRUE);

    // Row 2: ListView (takes remaining space minus bottom rows)
    y += ctrlH + 8;
    int bottomH = btnH + 8 + chkH + M;     // refresh row + checkbox row + margin
    int lvH = H - y - bottomH - 4;
    if (lvH < 60) lvH = 60;
    MoveWindow(g_hListView, M, y, W - M * 2, lvH, TRUE);
    ListView_SetColumnWidth(g_hListView, 0, W - M * 2 - 30);

    // Row 3: button + status
    y += lvH + 8;
    MoveWindow(g_hBtnRefresh, M, y, btnW, btnH, TRUE);
    MoveWindow(g_hLabelStatus, M + btnW + 12, y + 6,
               W - M * 2 - btnW - 12, ctrlH, TRUE);

    // Row 4: checkboxes
    y += btnH + 8;
    int chkW = (W - M * 2 - 12) / 2;       // split width evenly
    MoveWindow(g_hChkAutoStart, M,          y, chkW, chkH, TRUE);
    MoveWindow(g_hChkMinimized, M + chkW + 12, y, chkW, chkH, TRUE);
}

static void PopulateDeviceList()
{
    // Snapshot current selections so we can restore them after re-enumeration
    std::vector<std::wstring> previouslySelected;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        previouslySelected = g_selectedDeviceIds;
    }

    // Suppress LVN_ITEMCHANGED while we rebuild the list
    bool prevSuppress = g_bSuppressNotify;
    g_bSuppressNotify = true;

    ListView_DeleteAllItems(g_hListView);
    g_devices = EnumerateAudioDevices();

    for (int i = 0; i < static_cast<int>(g_devices.size()); ++i) {
        LVITEMW lvi   = {};
        lvi.mask      = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem     = i;
        lvi.pszText   = const_cast<LPWSTR>(g_devices[i].name.c_str());
        lvi.lParam    = static_cast<LPARAM>(i);
        ListView_InsertItem(g_hListView, &lvi);
    }

    // Restore checkmarks for devices that still exist
    for (int i = 0; i < static_cast<int>(g_devices.size()); ++i) {
        for (const auto& prevId : previouslySelected) {
            if (g_devices[i].id == prevId) {
                ListView_SetCheckState(g_hListView, i, TRUE);
                break;
            }
        }
    }

    g_bSuppressNotify = prevSuppress;

    // Single sync of shared state + save
    UpdateSelectedDevices();
}

static void UpdateSelectedDevices()
{
    std::vector<std::wstring> selected;
    int count = ListView_GetItemCount(g_hListView);
    for (int i = 0; i < count; ++i) {
        if (ListView_GetCheckState(g_hListView, i)) {
            int idx = static_cast<int>(i);
            if (idx >= 0 && idx < static_cast<int>(g_devices.size())) {
                selected.push_back(g_devices[idx].id);
            }
        }
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    g_selectedDeviceIds = std::move(selected);
}

// ===========================================================================
//  Settings persistence (INI file next to the EXE)
// ===========================================================================

static std::wstring GetSettingsPath()
{
    WCHAR exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring path(exePath);
    auto dot = path.rfind(L'.');
    if (dot != std::wstring::npos)
        path = path.substr(0, dot);
    path += L".ini";
    return path;
}

static void SaveSettings()
{
    std::wstring ini = GetSettingsPath();
    const wchar_t* file = ini.c_str();

    // Save target app (read directly from the edit control)
    wchar_t target[MAX_PATH] = {};
    if (g_hEditTarget)
        GetWindowTextW(g_hEditTarget, target, MAX_PATH);
    WritePrivateProfileStringW(L"Settings", L"TargetApp", target, file);

    // Save checkbox states
    bool startMin = (SendMessageW(g_hChkMinimized, BM_GETCHECK, 0, 0)
                     == BST_CHECKED);
    WritePrivateProfileStringW(L"Settings", L"StartMinimized",
                               startMin ? L"1" : L"0", file);

    bool autoStart = (SendMessageW(g_hChkAutoStart, BM_GETCHECK, 0, 0)
                      == BST_CHECKED);
    WritePrivateProfileStringW(L"Settings", L"AutoStart",
                               autoStart ? L"1" : L"0", file);

    // Save selected device IDs
    std::vector<std::wstring> selected;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        selected = g_selectedDeviceIds;
    }

    // Clear old device entries by deleting the section, then rewrite
    WritePrivateProfileStringW(L"Devices", nullptr, nullptr, file);

    wchar_t countStr[16] = {};
    swprintf_s(countStr, L"%d", static_cast<int>(selected.size()));
    WritePrivateProfileStringW(L"Devices", L"Count", countStr, file);

    for (int i = 0; i < static_cast<int>(selected.size()); ++i) {
        wchar_t key[32] = {};
        swprintf_s(key, L"Id%d", i);
        WritePrivateProfileStringW(L"Devices", key,
                                   selected[i].c_str(), file);
    }
}

static void LoadSettings()
{
    std::wstring ini = GetSettingsPath();
    const wchar_t* file = ini.c_str();

    // If the file doesn't exist yet, set defaults and return
    if (GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES) {
        // Default: start minimized is ON
        SendMessageW(g_hChkMinimized, BM_SETCHECK, BST_CHECKED, 0);
        return;
    }

    // Load target app
    wchar_t target[MAX_PATH] = {};
    GetPrivateProfileStringW(L"Settings", L"TargetApp", L"Discord.exe",
                             target, MAX_PATH, file);
    SetWindowTextW(g_hEditTarget, target);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_targetApp = target;
    }

    // Load checkbox states
    int startMin = GetPrivateProfileIntW(
        L"Settings", L"StartMinimized", 1, file);
    SendMessageW(g_hChkMinimized, BM_SETCHECK,
                 startMin ? BST_CHECKED : BST_UNCHECKED, 0);

    int autoStart = GetPrivateProfileIntW(
        L"Settings", L"AutoStart", 0, file);
    SendMessageW(g_hChkAutoStart, BM_SETCHECK,
                 autoStart ? BST_CHECKED : BST_UNCHECKED, 0);

    // Sync the registry with the saved auto-start state, in case the user
    // moved the EXE since last run (updates the registry path).
    SetAutoStart(autoStart != 0);

    // Load selected device IDs
    int count = GetPrivateProfileIntW(L"Devices", L"Count", 0, file);
    std::vector<std::wstring> savedIds;
    for (int i = 0; i < count; ++i) {
        wchar_t key[32] = {};
        swprintf_s(key, L"Id%d", i);
        wchar_t id[512] = {};
        GetPrivateProfileStringW(L"Devices", key, L"", id, 512, file);
        if (wcslen(id) > 0)
            savedIds.push_back(id);
    }

    // Check matching devices in the ListView
    for (int i = 0; i < static_cast<int>(g_devices.size()); ++i) {
        for (const auto& savedId : savedIds) {
            if (g_devices[i].id == savedId) {
                ListView_SetCheckState(g_hListView, i, TRUE);
                break;
            }
        }
    }
    // Sync shared state
    UpdateSelectedDevices();
}

// ===========================================================================
//  Auto-start (registry)
// ===========================================================================

static void SetAutoStart(bool enable)
{
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegRunKey,
                      0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS)
    {
        if (enable) {
            WCHAR exePath[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            RegSetValueExW(hKey, kRegValueName, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(exePath),
                static_cast<DWORD>((wcslen(exePath) + 1) * sizeof(WCHAR)));
        } else {
            RegDeleteValueW(hKey, kRegValueName);
        }
        RegCloseKey(hKey);
    }
}

static bool GetAutoStart()
{
    HKEY hKey = nullptr;
    bool result = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegRunKey,
                      0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS)
    {
        result = (RegQueryValueExW(hKey, kRegValueName,
                                   nullptr, nullptr,
                                   nullptr, nullptr) == ERROR_SUCCESS);
        RegCloseKey(hKey);
    }
    return result;
}

// ===========================================================================
//  System tray
// ===========================================================================

static void AddTrayIcon(HWND hWnd)
{
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hWnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = g_hIconSmall;
    wcscpy_s(g_nid.szTip, L"Discord Device Muter");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void RemoveTrayIcon()
{
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void ShowTrayMenu(HWND hWnd)
{
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SHOW,
                IsWindowVisible(hWnd) ? L"Hide" : L"Show");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");

    // Required by TrackPopupMenu
    SetForegroundWindow(hWnd);
    UINT cmd = TrackPopupMenu(
        hMenu, TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x, pt.y, 0, hWnd, nullptr);
    DestroyMenu(hMenu);
    PostMessage(hWnd, WM_NULL, 0, 0);  // dismiss menu on click-away

    switch (cmd) {
    case ID_TRAY_SHOW:
        if (IsWindowVisible(hWnd)) {
            ShowWindow(hWnd, SW_HIDE);
        } else {
            ShowWindow(hWnd, SW_SHOW);
            ShowWindow(hWnd, SW_RESTORE);
            SetForegroundWindow(hWnd);
        }
        break;
    case ID_TRAY_EXIT:
        DestroyWindow(hWnd);
        break;
    }
}

// ===========================================================================
//  Background monitor thread
// ===========================================================================

DWORD WINAPI MonitorThreadProc(LPVOID /*param*/)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // Run immediately, then every 5 seconds
    do {
        // Snapshot shared state
        std::vector<std::wstring> deviceIds;
        std::wstring target;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            deviceIds = g_selectedDeviceIds;
            target    = g_targetApp;
        }

        if (!deviceIds.empty() && !target.empty()) {
            int foundCount = 0;
            for (const auto& id : deviceIds) {
                if (MuteAppOnDevice(id, target))
                    ++foundCount;
            }
            PostMessageW(g_hWnd, WM_UPDATE_STATUS,
                         static_cast<WPARAM>(foundCount),
                         static_cast<LPARAM>(deviceIds.size()));
        }

    } while (WaitForSingleObject(g_hStopEvent, 5000) == WAIT_TIMEOUT);

    CoUninitialize();
    return 0;
}

// ===========================================================================
//  Custom icon generation (Speaker with mute slash)
// ===========================================================================

static HICON CreateMuteIcon(int size)
{
    HDC hdcScreen = GetDC(nullptr);
    HDC hdcColor = CreateCompatibleDC(hdcScreen);
    HDC hdcMask = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = size;
    bmi.bmiHeader.biHeight = size;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hbmColor = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP hbmMask = CreateBitmap(size, size, 1, 1, nullptr);

    HGDIOBJ oldColor = SelectObject(hdcColor, hbmColor);
    HGDIOBJ oldMask = SelectObject(hdcMask, hbmMask);

    // Color background (black initially)
    RECT rc = { 0, 0, size, size };
    HBRUSH hbrBlack = (HBRUSH)GetStockObject(BLACK_BRUSH);
    FillRect(hdcColor, &rc, hbrBlack);

    // Mask background (1 = transparent in icon mask)
    HBRUSH hbrWhite = (HBRUSH)GetStockObject(WHITE_BRUSH);
    FillRect(hdcMask, &rc, hbrWhite);

    // Drawing coordinates scaled to size
    // Speaker box and cone
    int x1 = size * 2 / 16;
    int x2 = size * 6 / 16;
    int x3 = size * 10 / 16;
    int yTop = size * 3 / 16;
    int yMidTop = size * 5 / 16;
    int yMidBottom = size * 11 / 16;
    int yBottom = size * 13 / 16;

    POINT speakerPts[6] = {
        { x1, yMidTop },
        { x2, yMidTop },
        { x3, yTop },
        { x3, yBottom },
        { x2, yMidBottom },
        { x1, yMidBottom }
    };

    // Draw speaker body in white on color DC
    HBRUSH hbrSpeaker = CreateSolidBrush(RGB(220, 220, 220));
    HPEN hpenSpeaker = CreatePen(PS_SOLID, 1, RGB(220, 220, 220));
    HGDIOBJ oldBrushColor = SelectObject(hdcColor, hbrSpeaker);
    HGDIOBJ oldPenColor = SelectObject(hdcColor, hpenSpeaker);
    Polygon(hdcColor, speakerPts, 6);

    // Clear mask where speaker is drawn (0 = opaque)
    HGDIOBJ oldBrushMask = SelectObject(hdcMask, hbrBlack);
    HGDIOBJ oldPenMask = SelectObject(hdcMask, GetStockObject(BLACK_PEN));
    Polygon(hdcMask, speakerPts, 6);

    // Draw mute slash (Red on color, opaque on mask)
    int penW = (size >= 32) ? 3 : 2;
    HPEN hpenRed = CreatePen(PS_SOLID, penW, RGB(235, 50, 50));
    SelectObject(hdcColor, hpenRed);

    int slashPad = size * 2 / 16;
    MoveToEx(hdcColor, size - slashPad, slashPad, nullptr);
    LineTo(hdcColor, slashPad, size - slashPad);

    HPEN hpenMaskSlash = CreatePen(PS_SOLID, penW, RGB(0, 0, 0));
    SelectObject(hdcMask, hpenMaskSlash);
    MoveToEx(hdcMask, size - slashPad, slashPad, nullptr);
    LineTo(hdcMask, slashPad, size - slashPad);

    // Clean up GDI objects
    SelectObject(hdcColor, oldBrushColor);
    SelectObject(hdcColor, oldPenColor);
    SelectObject(hdcMask, oldBrushMask);
    SelectObject(hdcMask, oldPenMask);
    DeleteObject(hbrSpeaker);
    DeleteObject(hpenSpeaker);
    DeleteObject(hpenRed);
    DeleteObject(hpenMaskSlash);

    SelectObject(hdcColor, oldColor);
    SelectObject(hdcMask, oldMask);
    DeleteDC(hdcColor);
    DeleteDC(hdcMask);
    ReleaseDC(nullptr, hdcScreen);

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = hbmColor;
    ii.hbmMask = hbmMask;

    HICON hIcon = CreateIconIndirect(&ii);

    DeleteObject(hbmColor);
    DeleteObject(hbmMask);

    return hIcon;
}

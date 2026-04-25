#pragma once
// app_common.h - Shared types, globals, and utilities for MuteDiscordDevice.
// All other src/ modules include this header.

#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <atlbase.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <string>
#include <vector>
#include <unordered_map>

#include "../mdd_pure.h"
#include "../version.h"
#include "../resource.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "psapi.lib")

// HSTRING is defined in <winstring.h>; guard against redefinition
#if !defined(HSTRING_DEFINED)
typedef struct HSTRING__* HSTRING;
#define HSTRING_DEFINED
#endif

// ---------- Message IDs ----------

#define WM_TRAYICON (WM_USER + 1)
static UINT WM_TASKBARCREATED = 0;
static UINT WM_REQUEST_SHUTDOWN = 0;

#define ID_TRAY_EXIT 1001
#define ID_TRAY_CONFIGURE 1002
#define ID_TRAY_DIAGNOSE 1003
#define ID_TRAY_ROUTETARGET 1004

#define IDC_ROUTE_COMBO  4001
#define IDC_ROUTE_OK     4002
#define IDC_ROUTE_CANCEL 4003

#define IDC_CONFIG_LIST 2001
#define IDC_CONFIG_OK   2002
#define IDC_CONFIG_CANCEL 2003
#define IDC_CONFIG_SESSION_BTN 2004

#define IDC_SESSION_OK            3001
#define IDC_SESSION_CANCEL        3002
#define IDC_SESSION_ALL           3003
#define IDC_SESSION_ORDINAL_RADIO 3004
#define IDC_SESSION_ORDINAL_EDIT  3005
#define IDC_SESSION_STREAM        3006
#define IDC_SESSION_FIRST_RADIO   3100  // +index per session
#define IDC_SESSION_FIRST_TESTBTN 3300  // +index per session

// ---------- Window class names ----------

static const char* kTrayClassName = "MuteDiscordTrayClass";
static const wchar_t* kConfigClassName = L"MuteDiscordConfigClass";
static const wchar_t* kRouteTargetClassName = L"MuteDiscordRouteTargetClass";
static const wchar_t* kSessionPickerClassName = L"MuteDiscordSessionPickerClass";

// ---------- Shutdown message ----------

static const wchar_t* kShutdownMessageName = L"MuteDiscordDevice_RequestShutdown";

// ---------- Global state ----------

extern HINSTANCE g_hInst;
extern HWND g_hWnd;
extern NOTIFYICONDATAA g_nid;
extern HANDLE g_workerThread;
extern HANDLE g_exitEvent;
extern HWND g_hConfigWnd;
extern bool debugMode;

// ---------- DiscordSession data model ----------

struct DiscordSession {
    CComPtr<IAudioSessionControl> pControl;
    CComPtr<IAudioSessionControl2> pControl2;
    DWORD pid = 0;
    std::wstring sessionIdentifier;
    std::wstring sessionInstanceIdentifier;
    std::wstring displayName;
    std::wstring iconPath;
    GUID groupingParam = GUID_NULL;
    int ordinalOnDevice = 0;
};

// ---------- Utility declarations ----------

/// Check if a command-line argument is present.
bool HasCommandLineArg(int argc, char* argv[], const char* arg);

/// Set the working directory to the folder containing the executable.
void SetWorkingDirectoryToExeFolder();

/// Request a running instance to shut down via registered window message.
bool RequestRunningInstanceShutdown();

/// Get the process name (executable file) for a given PID.
std::wstring GetProcessName(DWORD pid);

/// Get the full image path for a given PID.
std::wstring GetProcessImagePath(DWORD pid);

/// Check if Discord.exe is currently running.
bool isDiscordRunning();

/// Dialog helper: check if owner window should be disabled during modal dialog.
bool ShouldDisableOwnerWindow(HWND owner);

/// Dialog helper: restore owner window after modal dialog closes.
void RestoreOwnerWindow(HWND owner);

// ---------- Forward declarations for other modules ----------

// audio_device
CComPtr<IMMDevice> FindDeviceByName(const std::wstring& deviceName, IMMDeviceEnumerator* pEnumIn = nullptr);
std::vector<DiscordSession> EnumerateDiscordSessions(const std::wstring& deviceName, IMMDeviceEnumerator* pEnum = nullptr);
std::unordered_map<std::wstring, std::vector<DWORD>> BuildDiscordPidsByDevice(IMMDeviceEnumerator* pEnum = nullptr);
int MuteDiscordOnDevice(const DeviceConfig& cfg, const std::vector<DWORD>& otherPids, IMMDeviceEnumerator* pEnum = nullptr);
void UnmuteDiscordSessionsOnDevice(const std::wstring& deviceName, IMMDeviceEnumerator* pEnum = nullptr);
std::vector<std::wstring> EnumerateRenderDevices(IMMDeviceEnumerator* pEnumIn = nullptr);

// app_config
std::wstring LoadRouteTargetFromFile();
bool SaveRouteTargetToFile(const std::wstring& deviceName);
std::vector<DeviceConfig> LoadDevicesFromFile(const std::wstring& filename);
bool SaveDevicesToFile(const std::wstring& filename, const std::vector<DeviceConfig>& devices);
void SeedDefaultDevicesIfFirstRun();
int checkToMute();
void RunDiagnostic(HWND owner);

// routing
void RouteDiscordAudioOutput(const std::wstring& targetDeviceFriendlyName, IMMDeviceEnumerator* pEnum);

// tray
void AddTrayIcon(HWND hWnd);
void RemoveTrayIcon(HWND hWnd);
void ShowTrayMenu(HWND hWnd, POINT pt);
LRESULT CALLBACK TrayWndProc(HWND, UINT, WPARAM, LPARAM);

// config_dialog
void ShowConfigDialog(HWND owner);
LRESULT CALLBACK ConfigWndProc(HWND, UINT, WPARAM, LPARAM);

// session_picker
bool ShowSessionPicker(HWND owner, const std::wstring& deviceName, DeviceConfig& ioCfg);
LRESULT CALLBACK SessionPickerWndProc(HWND, UINT, WPARAM, LPARAM);
void ShowRouteTargetDialog(HWND owner);
LRESULT CALLBACK RouteTargetWndProc(HWND, UINT, WPARAM, LPARAM);

// wmi_monitor
DWORD WINAPI WorkerThreadProc(LPVOID);

#include "app_common.h"
#include <sstream>
#include <unordered_map>

// ---------- Process name cache (Fix 3.2) ----------

static std::unordered_map<DWORD, bool> g_processNameCache;

void ClearProcessNameCache() {
    g_processNameCache.clear();
}

bool IsDiscordProcess(DWORD pid) {
    auto it = g_processNameCache.find(pid);
    if (it != g_processNameCache.end()) return it->second;

    bool isDiscord = (_wcsicmp(GetProcessName(pid).c_str(), L"Discord.exe") == 0);
    g_processNameCache[pid] = isDiscord;
    return isDiscord;
}

bool HasCommandLineArg(int argc, char* argv[], const char* arg) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], arg) == 0) {
            return true;
        }
    }
    return false;
}

void SetWorkingDirectoryToExeFolder() {
    wchar_t exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;

    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (!slash) return;
    *slash = L'\0';
    SetCurrentDirectoryW(exePath);
}

bool RequestRunningInstanceShutdown() {
    UINT shutdownMessage = WM_REQUEST_SHUTDOWN;
    if (!shutdownMessage) {
        shutdownMessage = RegisterWindowMessageW(kShutdownMessageName);
    }
    if (!shutdownMessage) return false;

    bool signaled = false;

    HWND hTray = FindWindowW(kTrayClassName, nullptr);
    while (hTray) {
        PostMessageW(hTray, shutdownMessage, 0, 0);
        signaled = true;
        hTray = FindWindowExW(nullptr, hTray, kTrayClassName, nullptr);
    }

    HWND hConfig = FindWindowW(kConfigClassName, nullptr);
    while (hConfig) {
        PostMessageW(hConfig, WM_CLOSE, 0, 0);
        signaled = true;
        hConfig = FindWindowExW(nullptr, hConfig, kConfigClassName, nullptr);
    }

    HWND hRoute = FindWindowW(kRouteTargetClassName, nullptr);
    while (hRoute) {
        PostMessageW(hRoute, WM_CLOSE, 0, 0);
        signaled = true;
        hRoute = FindWindowExW(nullptr, hRoute, kRouteTargetClassName, nullptr);
    }

    return signaled;
}

std::wstring GetProcessName(DWORD pid) {
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return L"";
    wchar_t buf[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(hProc, 0, buf, &size);
    CloseHandle(hProc);
    if (!ok) return L"";
    const wchar_t* slash = wcsrchr(buf, L'\\');
    return slash ? slash + 1 : buf;
}

std::wstring GetProcessImagePath(DWORD pid) {
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return L"";
    wchar_t buf[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(hProc, 0, buf, &size);
    CloseHandle(hProc);
    if (!ok) return L"";
    return buf;
}

bool isDiscordRunning() {
    bool found = false;
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);
    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            if (_wcsicmp(pe32.szExeFile, L"Discord.exe") == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
    return found;
}

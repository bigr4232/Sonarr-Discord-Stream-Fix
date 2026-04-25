// MuteDiscordDevice - Main entry point
//
// This file contains only WinMain and includes all module headers.
// All functionality has been extracted into src/ modules.

#include "src/app_common.h"

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    if (HasCommandLineArg(__argc, __argv, "--shutdown") || HasCommandLineArg(__argc, __argv, "--exit")) {
        return RequestRunningInstanceShutdown() ? 0 : 1;
    }

    SetWorkingDirectoryToExeFolder();

    g_hInst = hInstance;
    g_exitEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = kTrayClassName;
    RegisterClassA(&wc);

    WNDCLASSW cwc = { 0 };
    cwc.lpfnWndProc = ConfigWndProc;
    cwc.hInstance = hInstance;
    cwc.hCursor = LoadCursor(NULL, IDC_ARROW);
    cwc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    cwc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    if (!cwc.hIcon) cwc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    cwc.lpszClassName = kConfigClassName;
    RegisterClassW(&cwc);

    // Register window messages BEFORE creating the tray window so there is no
    // gap where an incoming shutdown broadcast could arrive with an unhandled
    // message ID. RegisterWindowMessageW is system-wide and thread-safe; all
    // instances of this app will see the same message ID for a given name.
    WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");
    WM_REQUEST_SHUTDOWN = RegisterWindowMessageW(kShutdownMessageName);

    // Title must be non-empty: on some Windows 11 builds, DefWindowProc returns
    // FALSE from WM_NCCREATE when lpszName is "" and the window is never
    // created. The string is not user-visible (the window is hidden).
    g_hWnd = CreateWindowA(wc.lpszClassName, "MuteDiscordTray", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    if (!g_hWnd) {
        return 1;
    }

    // Allow the broadcast messages to reach us even when running unelevated
    // alongside an elevated Explorer (UIPI blocks WM_USER+ range by default).
    ChangeWindowMessageFilterEx(g_hWnd, WM_TASKBARCREATED, MSGFLT_ALLOW, NULL);
    if (WM_REQUEST_SHUTDOWN) {
        ChangeWindowMessageFilterEx(g_hWnd, WM_REQUEST_SHUTDOWN, MSGFLT_ALLOW, NULL);
    }

    AddTrayIcon(g_hWnd);

    SeedDefaultDevicesIfFirstRun();

    g_workerThread = CreateThread(NULL, 0, WorkerThreadProc, NULL, 0, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (g_hConfigWnd && IsWindow(g_hConfigWnd) && IsDialogMessageW(g_hConfigWnd, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Give the worker a brief window to unblock from WMI / WASAPI calls.
    // If it can't exit in time (e.g. Restart Manager is waiting on us during
    // an installer upgrade), ExitProcess so we don't hang the installer.
    if (WaitForSingleObject(g_workerThread, 2000) != WAIT_OBJECT_0) {
        ExitProcess(0);
    }
    CloseHandle(g_workerThread);
    CloseHandle(g_exitEvent);

    CoUninitialize();
    return 0;
}

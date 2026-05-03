#include "app_common.h"

static HICON LoadTrayIcon() {
    int cx = GetSystemMetrics(SM_CXSMICON);
    int cy = GetSystemMetrics(SM_CYSMICON);
    HICON hIcon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR);
    if (hIcon) return hIcon;
    SHSTOCKICONINFO sii = { sizeof(SHSTOCKICONINFO) };
    if (SUCCEEDED(SHGetStockIconInfo(SIID_AUDIOFILES, SHGSI_ICON | SHGSI_SMALLICON, &sii))) {
        return sii.hIcon;
    }
    return LoadIcon(NULL, IDI_APPLICATION);
}

void AddTrayIcon(HWND hWnd) {
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadTrayIcon();
    wcscpy_s(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"MuteDiscordDevice v" MDD_VERSION_STRING);

    for (int attempt = 0; attempt < 10; ++attempt) {
        if (Shell_NotifyIconW(NIM_ADD, &g_nid)) return;
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        Sleep(500);
    }

    // All retries exhausted — notify the user so they know the app is running but broken.
    MessageBoxW(hWnd,
        L"Failed to add tray icon after 10 attempts.\n\n"
        L"The application is still running but you will not be able to interact with it.\n"
        L"Please restart the application or check Task Manager.",
        L"MuteDiscordDevice — Tray Icon Error",
        MB_OK | MB_ICONERROR);
}

void RemoveTrayIcon(HWND) {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void ShowAboutDialog(HWND owner) {
    MessageBoxW(owner,
        L"MuteDiscordDevice\n"
        L"Version " MDD_VERSION_W L"\n\n"
        L"Mutes Discord only on the audio devices you choose, so streamers\n"
        L"can keep Discord audible on headphones while keeping it off the\n"
        L"stream.",
        L"About MuteDiscordDevice",
        MB_OK | MB_ICONINFORMATION);
}

void ShowTrayMenu(HWND hWnd, POINT pt) {
    HMENU hMenu = CreatePopupMenu();
    InsertMenuW(hMenu, -1, MF_BYPOSITION, ID_TRAY_ROUTETARGET, L"Route Discord Audio...");
    InsertMenuW(hMenu, -1, MF_BYPOSITION, ID_TRAY_CONFIGURE,   L"Configure Mute Devices...");
    InsertMenuW(hMenu, -1, MF_BYPOSITION, ID_TRAY_DIAGNOSE,    L"Diagnose Discord Sessions...");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    InsertMenuW(hMenu, -1, MF_BYPOSITION, ID_TRAY_ABOUT,       L"About...");
    InsertMenuW(hMenu, -1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    InsertMenuW(hMenu, -1, MF_BYPOSITION, ID_TRAY_EXIT, L"Exit");
    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hWnd, NULL);
    PostMessage(hWnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
}

LRESULT CALLBACK TrayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (WM_TASKBARCREATED != 0 && msg == WM_TASKBARCREATED) {
        AddTrayIcon(hWnd);
        return 0;
    }
    if (WM_REQUEST_SHUTDOWN != 0 && msg == WM_REQUEST_SHUTDOWN) {
        RemoveTrayIcon(hWnd);
        if (g_exitEvent) SetEvent(g_exitEvent);
        DestroyWindow(hWnd);
        return 0;
    }
    switch (msg) {
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            ShowTrayMenu(hWnd, pt);
        }
        break;
    case WM_COMMAND:
        if (LOWORD(wParam) == ID_TRAY_EXIT) {
            SetEvent(g_exitEvent);
            DestroyWindow(hWnd);
        }
        else if (LOWORD(wParam) == ID_TRAY_ROUTETARGET) {
            ShowRouteTargetDialog(hWnd);
        }
        else if (LOWORD(wParam) == ID_TRAY_CONFIGURE) {
            ShowConfigDialog(hWnd);
        }
        else if (LOWORD(wParam) == ID_TRAY_DIAGNOSE) {
            RunDiagnostic(hWnd);
        }
        else if (LOWORD(wParam) == ID_TRAY_ABOUT) {
            ShowAboutDialog(hWnd);
        }
        break;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wParam) {
            RemoveTrayIcon(hWnd);
            if (g_exitEvent) SetEvent(g_exitEvent);
            DestroyWindow(hWnd);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        RemoveTrayIcon(hWnd);
        if (g_exitEvent) SetEvent(g_exitEvent);
        PostQuitMessage(0);
        break;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

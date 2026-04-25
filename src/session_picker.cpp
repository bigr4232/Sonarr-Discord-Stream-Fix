#include "app_common.h"

struct SessionPickerState {
    HWND hOwner;
    std::wstring deviceName;
    std::vector<DiscordSession> sessions;
    DeviceConfig* ioCfg;
    bool accepted = false;

    HWND hStreamRadio = nullptr;
    HWND hAllRadio = nullptr;
    std::vector<HWND> hSessionRadios;
    std::vector<HWND> hTestBtns;
    HWND hOrdinalRadio = nullptr;
    HWND hOrdinalEdit = nullptr;
    HWND hOk = nullptr;
    HWND hCancel = nullptr;
};

static void ToggleSessionMute(DiscordSession& s) {
    CComQIPtr<ISimpleAudioVolume> pVol(s.pControl);
    if (!pVol) return;
    BOOL currentlyMuted = FALSE;
    pVol->GetMute(&currentlyMuted);
    pVol->SetMute(currentlyMuted ? FALSE : TRUE, nullptr);
}

LRESULT CALLBACK SessionPickerWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SessionPickerState* st = reinterpret_cast<SessionPickerState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SessionPickerState* s = reinterpret_cast<SessionPickerState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));

        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        int y = 10;

        std::wstring header = L"Device: " + s->deviceName;
        HWND hHeader = CreateWindowW(L"STATIC", header.c_str(),
            WS_CHILD | WS_VISIBLE,
            10, y, 660, 20, hWnd, NULL, g_hInst, NULL);
        SendMessageW(hHeader, WM_SETFONT, (WPARAM)hFont, TRUE);
        y += 26;

        s->hAllRadio = CreateWindowW(L"BUTTON",
            L"Mute all Discord sessions on this device  [Recommended]",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            10, y, 660, 20, hWnd, (HMENU)(UINT_PTR)IDC_SESSION_ALL, g_hInst, NULL);
        SendMessageW(s->hAllRadio, WM_SETFONT, (WPARAM)hFont, TRUE);
        y += 26;

        s->hStreamRadio = CreateWindowW(L"BUTTON",
            L"Stream only (auto-detect) — only mutes when a Discord stream is live",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            10, y, 660, 20, hWnd, (HMENU)(UINT_PTR)IDC_SESSION_STREAM, g_hInst, NULL);
        SendMessageW(s->hStreamRadio, WM_SETFONT, (WPARAM)hFont, TRUE);
        y += 26;

        if (s->sessions.empty()) {
            HWND hInfo = CreateWindowW(L"STATIC",
                L"No Discord sessions found on this device.\n"
                L"Start Discord (and your stream if applicable) and reopen this picker.",
                WS_CHILD | WS_VISIBLE,
                30, y, 640, 40, hWnd, NULL, g_hInst, NULL);
            SendMessageW(hInfo, WM_SETFONT, (WPARAM)hFont, TRUE);
            y += 46;
        }

        for (size_t i = 0; i < s->sessions.size(); i++) {
            const auto& sess = s->sessions[i];
            std::wstring label = L"Session #" + std::to_wstring(sess.ordinalOnDevice) +
                L"  -  " + (sess.displayName.empty() ? L"(no name)" : sess.displayName) +
                L"  [PID " + std::to_wstring(sess.pid) + L"]";
            std::wstring fp = StableSessionFingerprint(sess.sessionIdentifier);
            if (!fp.empty()) {
                std::wstring shortFp = fp.size() > 40 ? fp.substr(0, 40) + L"..." : fp;
                label += L"\n    fp: " + shortFp;
            }

            HWND hRadio = CreateWindowW(L"BUTTON", label.c_str(),
                WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_MULTILINE,
                10, y, 560, 40,
                hWnd, (HMENU)(UINT_PTR)(IDC_SESSION_FIRST_RADIO + i), g_hInst, NULL);
            SendMessageW(hRadio, WM_SETFONT, (WPARAM)hFont, TRUE);
            s->hSessionRadios.push_back(hRadio);

            HWND hTest = CreateWindowW(L"BUTTON", L"Toggle mute",
                WS_CHILD | WS_VISIBLE,
                580, y + 8, 90, 24,
                hWnd, (HMENU)(UINT_PTR)(IDC_SESSION_FIRST_TESTBTN + i), g_hInst, NULL);
            SendMessageW(hTest, WM_SETFONT, (WPARAM)hFont, TRUE);
            s->hTestBtns.push_back(hTest);

            y += 46;
        }

        s->hOrdinalRadio = CreateWindowW(L"BUTTON", L"Ordinal fallback: mute the",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            10, y, 200, 22, hWnd, (HMENU)(UINT_PTR)IDC_SESSION_ORDINAL_RADIO, g_hInst, NULL);
        SendMessageW(s->hOrdinalRadio, WM_SETFONT, (WPARAM)hFont, TRUE);

        s->hOrdinalEdit = CreateWindowW(L"EDIT", L"1",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
            215, y, 40, 22, hWnd, (HMENU)(UINT_PTR)IDC_SESSION_ORDINAL_EDIT, g_hInst, NULL);
        SendMessageW(s->hOrdinalEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hTail = CreateWindowW(L"STATIC", L"th Discord session (0-indexed)",
            WS_CHILD | WS_VISIBLE,
            260, y + 2, 250, 22, hWnd, NULL, g_hInst, NULL);
        SendMessageW(hTail, WM_SETFONT, (WPARAM)hFont, TRUE);
        y += 34;

        s->hOk = CreateWindowW(L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            490, y, 80, 28, hWnd, (HMENU)(UINT_PTR)IDC_SESSION_OK, g_hInst, NULL);
        s->hCancel = CreateWindowW(L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE,
            580, y, 80, 28, hWnd, (HMENU)(UINT_PTR)IDC_SESSION_CANCEL, g_hInst, NULL);
        SendMessageW(s->hOk, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(s->hCancel, WM_SETFONT, (WPARAM)hFont, TRUE);

        if (s->ioCfg) {
            switch (s->ioCfg->filter) {
            case DeviceConfig::Filter::StreamOnly:
                SendMessageW(s->hStreamRadio, BM_SETCHECK, BST_CHECKED, 0);
                break;
            case DeviceConfig::Filter::All:
                SendMessageW(s->hAllRadio, BM_SETCHECK, BST_CHECKED, 0);
                break;
            case DeviceConfig::Filter::ByFingerprint: {
                bool found = false;
                for (size_t i = 0; i < s->sessions.size(); i++) {
                    if (StableSessionFingerprint(s->sessions[i].sessionIdentifier) == s->ioCfg->fingerprint) {
                        SendMessageW(s->hSessionRadios[i], BM_SETCHECK, BST_CHECKED, 0);
                        found = true;
                        break;
                    }
                }
                if (!found) SendMessageW(s->hStreamRadio, BM_SETCHECK, BST_CHECKED, 0);
                break;
            }
            case DeviceConfig::Filter::ByOrdinal:
                SendMessageW(s->hOrdinalRadio, BM_SETCHECK, BST_CHECKED, 0);
                if (s->ioCfg->ordinal >= 0) {
                    std::wstring v = std::to_wstring(s->ioCfg->ordinal);
                    SetWindowTextW(s->hOrdinalEdit, v.c_str());
                }
                break;
            }
        } else {
            SendMessageW(s->hAllRadio, BM_SETCHECK, BST_CHECKED, 0);
        }

        if (ShouldDisableOwnerWindow(s->hOwner)) EnableWindow(s->hOwner, FALSE);
        return 0;
    }
    case WM_COMMAND: {
        if (!st) break;
        WORD id = LOWORD(wParam);
        if (id == IDC_SESSION_OK) {
            if (st->ioCfg) {
                if (SendMessageW(st->hStreamRadio, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                    st->ioCfg->filter = DeviceConfig::Filter::StreamOnly;
                    st->ioCfg->fingerprint.clear();
                    st->ioCfg->ordinal = -1;
                } else if (SendMessageW(st->hAllRadio, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                    st->ioCfg->filter = DeviceConfig::Filter::All;
                    st->ioCfg->fingerprint.clear();
                    st->ioCfg->ordinal = -1;
                } else if (SendMessageW(st->hOrdinalRadio, BM_GETCHECK, 0, 0) == BST_CHECKED) {
                    wchar_t buf[32] = { 0 };
                    GetWindowTextW(st->hOrdinalEdit, buf, 32);
                    int ord = -1;
                    try { ord = std::stoi(buf); } catch (...) {}
                    if (ord < 0) {
                        MessageBoxW(hWnd, L"Ordinal must be a non-negative integer.", L"Session picker",
                            MB_OK | MB_ICONWARNING);
                        return 0;
                    }
                    st->ioCfg->filter = DeviceConfig::Filter::ByOrdinal;
                    st->ioCfg->ordinal = ord;
                    st->ioCfg->fingerprint.clear();
                } else {
                    int picked = -1;
                    for (size_t i = 0; i < st->hSessionRadios.size(); i++) {
                        if (SendMessageW(st->hSessionRadios[i], BM_GETCHECK, 0, 0) == BST_CHECKED) {
                            picked = (int)i;
                            break;
                        }
                    }
                    if (picked < 0) {
                        MessageBoxW(hWnd, L"Pick a session or one of the other options.", L"Session picker",
                            MB_OK | MB_ICONWARNING);
                        return 0;
                    }
                    std::wstring fp = StableSessionFingerprint(st->sessions[picked].sessionIdentifier);
                    if (fp.empty()) {
                        MessageBoxW(hWnd,
                            L"That session has no usable fingerprint. Use the ordinal option instead.",
                            L"Session picker", MB_OK | MB_ICONWARNING);
                        return 0;
                    }
                    st->ioCfg->filter = DeviceConfig::Filter::ByFingerprint;
                    st->ioCfg->fingerprint = fp;
                    st->ioCfg->ordinal = -1;
                }
                st->accepted = true;
            }
            DestroyWindow(hWnd);
            return 0;
        }
        if (id == IDC_SESSION_CANCEL) {
            DestroyWindow(hWnd);
            return 0;
        }
        if (id >= IDC_SESSION_FIRST_TESTBTN &&
            id < IDC_SESSION_FIRST_TESTBTN + (int)st->sessions.size()) {
            int idx = id - IDC_SESSION_FIRST_TESTBTN;
            ToggleSessionMute(st->sessions[idx]);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        if (st) RestoreOwnerWindow(st->hOwner);
        PostMessage(hWnd, WM_NULL, 0, 0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void RegisterSessionPickerClassOnce() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = SessionPickerWndProc;
    wc.hInstance = g_hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON));
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.lpszClassName = kSessionPickerClassName;
    RegisterClassW(&wc);
    registered = true;
}

bool ShowSessionPicker(HWND owner, const std::wstring& deviceName, DeviceConfig& ioCfg) {
    RegisterSessionPickerClassOnce();

    SessionPickerState state;
    state.hOwner = owner;
    state.deviceName = deviceName;
    state.sessions = EnumerateDiscordSessions(deviceName);
    state.ioCfg = &ioCfg;

    int width = 700;
    int height = 180 + (int)state.sessions.size() * 46 + (state.sessions.empty() ? 60 : 0);
    if (height < 260) height = 260;
    int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    HWND hWnd = CreateWindowExW(WS_EX_DLGMODALFRAME,
        kSessionPickerClassName,
        L"Pick Discord session to mute",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, width, height,
        owner, NULL, g_hInst, &state);
    if (!hWnd) return false;
    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    MSG msg;
    while (IsWindow(hWnd)) {
        BOOL r = GetMessageW(&msg, NULL, 0, 0);
        if (r == 0 || r == -1) break;
        if (IsDialogMessageW(hWnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return state.accepted;
}

// ---------- Route target picker dialog ----------

struct RouteTargetState {
    HWND hCombo;
    HWND hOwner;
    bool accepted = false;
    std::wstring selected;
};

LRESULT CALLBACK RouteTargetWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    RouteTargetState* state = reinterpret_cast<RouteTargetState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        RouteTargetState* s = reinterpret_cast<RouteTargetState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));

        RECT rc; GetClientRect(hWnd, &rc);
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        HWND hLabel = CreateWindowW(L"STATIC", L"Route all Discord audio to:",
            WS_CHILD | WS_VISIBLE, 10, 14, rc.right - 20, 20, hWnd, NULL, g_hInst, NULL);
        SendMessageW(hLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        s->hCombo = CreateWindowW(L"COMBOBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST,
            10, 40, rc.right - 20, 200, hWnd, (HMENU)IDC_ROUTE_COMBO, g_hInst, NULL);
        SendMessageW(s->hCombo, WM_SETFONT, (WPARAM)hFont, TRUE);

        SendMessageW(s->hCombo, CB_ADDSTRING, 0, (LPARAM)L"(none - disable routing)");
        std::wstring current = LoadRouteTargetFromFile();
        int selIdx = 0;
        int idx = 1;
        for (const auto& name : EnumerateRenderDevices()) {
            SendMessageW(s->hCombo, CB_ADDSTRING, 0, (LPARAM)name.c_str());
            if (!current.empty() && name == current) selIdx = idx;
            idx++;
        }
        SendMessageW(s->hCombo, CB_SETCURSEL, selIdx, 0);

        HWND hOk = CreateWindowW(L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            rc.right - 180, rc.bottom - 40, 80, 28,
            hWnd, (HMENU)IDC_ROUTE_OK, g_hInst, NULL);
        HWND hCancel = CreateWindowW(L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE,
            rc.right - 90, rc.bottom - 40, 80, 28,
            hWnd, (HMENU)IDC_ROUTE_CANCEL, g_hInst, NULL);
        SendMessageW(hOk, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(hCancel, WM_SETFONT, (WPARAM)hFont, TRUE);

        if (ShouldDisableOwnerWindow(s->hOwner)) EnableWindow(s->hOwner, FALSE);
        return 0;
    }
    case WM_COMMAND: {
        if (!state) break;
        WORD id = LOWORD(wParam);
        if (id == IDC_ROUTE_OK) {
            int sel = (int)SendMessageW(state->hCombo, CB_GETCURSEL, 0, 0);
            if (sel == 0) {
                state->selected = L"";
            } else {
                wchar_t buf[512] = {};
                SendMessageW(state->hCombo, CB_GETLBTEXT, sel, (LPARAM)buf);
                state->selected = buf;
            }
            state->accepted = true;
            DestroyWindow(hWnd);
        } else if (id == IDC_ROUTE_CANCEL) {
            DestroyWindow(hWnd);
        }
        break;
    }
    case WM_DESTROY:
        if (state) RestoreOwnerWindow(state->hOwner);
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void ShowRouteTargetDialog(HWND owner) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = RouteTargetWndProc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON));
        if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
        wc.lpszClassName = kRouteTargetClassName;
        RegisterClassW(&wc);
        registered = true;
    }

    RouteTargetState state;
    state.hOwner = owner;

    int width = 500, height = 150;
    int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    HWND hWnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kRouteTargetClassName,
        L"Route Discord Audio",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, width, height, owner, NULL, g_hInst, &state);
    if (!hWnd) return;

    MSG msg;
    bool sawQuit = false;
    while (IsWindow(hWnd)) {
        BOOL r = GetMessageW(&msg, NULL, 0, 0);
        if (r == 0) {
            sawQuit = true;
            break;
        }
        if (r == -1) break;
        if (IsDialogMessageW(hWnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (sawQuit) {
        PostQuitMessage(0);
    }

    if (state.accepted) {
        SaveRouteTargetToFile(state.selected);
        if (!state.selected.empty()) {
            CComPtr<IMMDeviceEnumerator> pEnum;
            if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                CLSCTX_ALL, IID_PPV_ARGS(&pEnum)))) {
                RouteDiscordAudioOutput(state.selected, pEnum);
            }
        }
    }
}

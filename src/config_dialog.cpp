#include "app_common.h"
#include <algorithm>

struct ConfigState {
    HWND hList;
    HWND hOk;
    HWND hCancel;
    HWND hSessionBtn;
    HWND hOwner;
};

bool ShouldDisableOwnerWindow(HWND owner) {
    return owner && IsWindow(owner) && IsWindowVisible(owner);
}

void RestoreOwnerWindow(HWND owner) {
    if (!ShouldDisableOwnerWindow(owner)) return;
    EnableWindow(owner, TRUE);
    if (IsIconic(owner)) ShowWindow(owner, SW_RESTORE);
    SetForegroundWindow(owner);
}

static void DeleteRowConfigs(HWND hList) {
    int count = ListView_GetItemCount(hList);
    for (int i = 0; i < count; i++) {
        LVITEMW item = { 0 };
        item.iItem = i;
        item.mask = LVIF_PARAM;
        if (ListView_GetItem(hList, &item) && item.lParam) {
            delete reinterpret_cast<DeviceConfig*>(item.lParam);
        }
    }
}

static void PopulateConfigList(HWND hList) {
    DeleteRowConfigs(hList);
    ListView_DeleteAllItems(hList);

    std::vector<std::wstring> enumerated = EnumerateRenderDevices();
    std::vector<DeviceConfig> configured = LoadDevicesFromFile(L"devices.txt");

    auto findConfigured = [&](const std::wstring& name) -> DeviceConfig* {
        for (auto& c : configured) if (c.name == name) return &c;
        return nullptr;
    };

    int row = 0;
    for (const auto& name : enumerated) {
        DeviceConfig* existing = findConfigured(name);
        DeviceConfig* rowCfg = new DeviceConfig();
        if (existing) *rowCfg = *existing;
        else rowCfg->name = name;
        rowCfg->offline = false;

        LVITEMW item = { 0 };
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = row;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(name.c_str());
        item.lParam = reinterpret_cast<LPARAM>(rowCfg);
        ListView_InsertItem(hList, &item);

        std::wstring summary = FilterSummary(*rowCfg);
        ListView_SetItemText(hList, row, 1, const_cast<LPWSTR>(summary.c_str()));

        ListView_SetCheckState(hList, row, existing != nullptr);
        row++;
    }

    for (const auto& c : configured) {
        if (std::find(enumerated.begin(), enumerated.end(), c.name) != enumerated.end()) continue;
        DeviceConfig* rowCfg = new DeviceConfig(c);
        rowCfg->offline = true;
        std::wstring display = c.name + L" (offline)";

        LVITEMW item = { 0 };
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = row;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(display.c_str());
        item.lParam = reinterpret_cast<LPARAM>(rowCfg);
        ListView_InsertItem(hList, &item);

        std::wstring summary = FilterSummary(*rowCfg);
        ListView_SetItemText(hList, row, 1, const_cast<LPWSTR>(summary.c_str()));

        ListView_SetCheckState(hList, row, TRUE);
        row++;
    }
}

static std::vector<DeviceConfig> CollectCheckedConfigs(HWND hList) {
    std::vector<DeviceConfig> result;
    int count = ListView_GetItemCount(hList);
    for (int i = 0; i < count; i++) {
        if (!ListView_GetCheckState(hList, i)) continue;
        LVITEMW item = { 0 };
        item.iItem = i;
        item.mask = LVIF_PARAM;
        ListView_GetItem(hList, &item);
        if (!item.lParam) continue;
        DeviceConfig* cfg = reinterpret_cast<DeviceConfig*>(item.lParam);
        result.push_back(*cfg);
    }
    return result;
}

static DeviceConfig* GetRowConfig(HWND hList, int row) {
    LVITEMW item = { 0 };
    item.iItem = row;
    item.mask = LVIF_PARAM;
    if (!ListView_GetItem(hList, &item)) return nullptr;
    return reinterpret_cast<DeviceConfig*>(item.lParam);
}

LRESULT CALLBACK ConfigWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    ConfigState* state = reinterpret_cast<ConfigState*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ConfigState* s = new ConfigState{ 0 };
        s->hOwner = reinterpret_cast<HWND>(cs->lpCreateParams);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));

        RECT rc;
        GetClientRect(hWnd, &rc);

        s->hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
            10, 10, rc.right - 20, rc.bottom - 60,
            hWnd, (HMENU)IDC_CONFIG_LIST, g_hInst, NULL);
        if (!s->hList) { delete s; SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0); return -1; }
        ListView_SetExtendedListViewStyle(s->hList,
            LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);

        LVCOLUMNW col0 = { 0 };
        col0.mask = LVCF_TEXT | LVCF_WIDTH;
        col0.pszText = const_cast<LPWSTR>(L"Device");
        col0.cx = (rc.right - 50) * 2 / 3;
        ListView_InsertColumn(s->hList, 0, &col0);

        LVCOLUMNW col1 = { 0 };
        col1.mask = LVCF_TEXT | LVCF_WIDTH;
        col1.pszText = const_cast<LPWSTR>(L"Discord session");
        col1.cx = (rc.right - 50) / 3;
        ListView_InsertColumn(s->hList, 1, &col1);

        s->hSessionBtn = CreateWindowW(L"BUTTON", L"Session...",
            WS_CHILD | WS_VISIBLE,
            10, rc.bottom - 40, 100, 28,
            hWnd, (HMENU)IDC_CONFIG_SESSION_BTN, g_hInst, NULL);
        s->hOk = CreateWindowW(L"BUTTON", L"OK",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            rc.right - 180, rc.bottom - 40, 80, 28,
            hWnd, (HMENU)IDC_CONFIG_OK, g_hInst, NULL);
        s->hCancel = CreateWindowW(L"BUTTON", L"Cancel",
            WS_CHILD | WS_VISIBLE,
            rc.right - 90, rc.bottom - 40, 80, 28,
            hWnd, (HMENU)IDC_CONFIG_CANCEL, g_hInst, NULL);

        if (!s->hSessionBtn || !s->hOk || !s->hCancel) {
            delete s;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
            return -1;
        }

        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        SendMessageW(s->hList, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(s->hSessionBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(s->hOk, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(s->hCancel, WM_SETFONT, (WPARAM)hFont, TRUE);

        PopulateConfigList(s->hList);

        if (ShouldDisableOwnerWindow(s->hOwner)) EnableWindow(s->hOwner, FALSE);
        return 0;
    }
    case WM_SIZE: {
        if (!state) break;
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);
        SetWindowPos(state->hList, NULL, 10, 10, w - 20, h - 60, SWP_NOZORDER);
        SetWindowPos(state->hSessionBtn, NULL, 10, h - 40, 100, 28, SWP_NOZORDER);
        SetWindowPos(state->hOk, NULL, w - 180, h - 40, 80, 28, SWP_NOZORDER);
        SetWindowPos(state->hCancel, NULL, w - 90, h - 40, 80, 28, SWP_NOZORDER);
        ListView_SetColumnWidth(state->hList, 0, (w - 50) * 2 / 3);
        ListView_SetColumnWidth(state->hList, 1, (w - 50) / 3);
        return 0;
    }
    case WM_COMMAND: {
        if (!state) break;
        WORD id = LOWORD(wParam);
        if (id == IDC_CONFIG_OK) {
            std::vector<DeviceConfig> previous = LoadDevicesFromFile(L"devices.txt");
            std::vector<DeviceConfig> checked = CollectCheckedConfigs(state->hList);
            if (!SaveDevicesToFile(L"devices.txt", checked)) {
                MessageBoxW(hWnd, L"Failed to save devices.txt", L"Error",
                    MB_OK | MB_ICONERROR);
                return 0;
            }
            DestroyWindow(hWnd);
            if (isDiscordRunning()) {
                std::vector<std::wstring> devicesToReset;
                for (const auto& oldCfg : previous) {
                    auto it = std::find_if(checked.begin(), checked.end(),
                        [&](const DeviceConfig& cfg) { return cfg.name == oldCfg.name; });
                    if (it == checked.end() ||
                        SerializeDeviceConfig(*it) != SerializeDeviceConfig(oldCfg)) {
                        devicesToReset.push_back(oldCfg.name);
                    }
                }

                if (!devicesToReset.empty()) {
                    CComPtr<IMMDeviceEnumerator> pEnum;
                    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                        IID_PPV_ARGS(&pEnum)))) {
                        for (const auto& deviceName : devicesToReset) {
                            UnmuteDiscordSessionsOnDevice(deviceName, pEnum);
                        }
                    }
                }
                checkToMute();
            }
            return 0;
        }
        if (id == IDC_CONFIG_CANCEL) {
            DestroyWindow(hWnd);
            return 0;
        }
        if (id == IDC_CONFIG_SESSION_BTN) {
            int sel = ListView_GetNextItem(state->hList, -1, LVNI_SELECTED);
            if (sel < 0) {
                MessageBoxW(hWnd, L"Select a device row first.", L"Session picker",
                    MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            DeviceConfig* cfg = GetRowConfig(state->hList, sel);
            if (!cfg) return 0;
            if (cfg->offline) {
                MessageBoxW(hWnd,
                    L"This device is offline. Plug it in / enable it to pick sessions.",
                    L"Session picker", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            if (ShowSessionPicker(hWnd, cfg->name, *cfg)) {
                std::wstring summary = FilterSummary(*cfg);
                ListView_SetItemText(state->hList, sel, 1, const_cast<LPWSTR>(summary.c_str()));
                ListView_SetCheckState(state->hList, sel, TRUE);
            }
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        if (state) {
            DeleteRowConfigs(state->hList);
            RestoreOwnerWindow(state->hOwner);
            delete state;
            SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);
        }
        return 0;
    case WM_NCDESTROY:
        g_hConfigWnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void ShowConfigDialog(HWND owner) {
    if (g_hConfigWnd && !IsWindow(g_hConfigWnd)) {
        g_hConfigWnd = nullptr;
    }

    if (g_hConfigWnd) {
        if (IsIconic(g_hConfigWnd)) {
            ShowWindow(g_hConfigWnd, SW_RESTORE);
        } else {
            ShowWindow(g_hConfigWnd, SW_SHOW);
        }
        SetForegroundWindow(g_hConfigWnd);
        return;
    }

    int width = 700;
    int height = 480;
    int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    g_hConfigWnd = CreateWindowExW(0, kConfigClassName,
        L"Configure Devices to Mute",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE | WS_THICKFRAME,
        x, y, width, height,
        owner, NULL, g_hInst, owner);
    if (g_hConfigWnd) {
        ShowWindow(g_hConfigWnd, SW_SHOW);
        UpdateWindow(g_hConfigWnd);
    }
}

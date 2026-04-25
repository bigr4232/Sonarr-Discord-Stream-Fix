#include "app_common.h"
#include <mutex>

// ---------- Global state definitions ----------

HINSTANCE g_hInst = nullptr;
HWND g_hWnd = nullptr;
NOTIFYICONDATAW g_nid = { 0 };
HANDLE g_workerThread = nullptr;
HANDLE g_exitEvent = nullptr;
HWND g_hConfigWnd = nullptr;
std::atomic<bool> debugMode{false};

// ---------- Cached device enumerator (Fix 3.1) ----------

static CComPtr<IMMDeviceEnumerator> g_deviceEnumerator;
static std::once_flag g_enumInitFlag;

IMMDeviceEnumerator* GetCachedDeviceEnumerator() {
    std::call_once(g_enumInitFlag, []() {
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                         IID_PPV_ARGS(&g_deviceEnumerator));
    });
    return g_deviceEnumerator.p;
}

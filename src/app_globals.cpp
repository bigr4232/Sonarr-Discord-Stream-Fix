#include "app_common.h"

// ---------- Global state definitions ----------

HINSTANCE g_hInst = nullptr;
HWND g_hWnd = nullptr;
NOTIFYICONDATAA g_nid = { 0 };
HANDLE g_workerThread = nullptr;
HANDLE g_exitEvent = nullptr;
HWND g_hConfigWnd = nullptr;
bool debugMode = false;

#include "app_common.h"
#include <fstream>
#include <sstream>
#include <chrono>
#include <ctime>

// ---------- Config I/O ----------

std::wstring LoadRouteTargetFromFile() {
    std::wifstream file(L"route_target.txt");
    if (!file.is_open()) return L"";
    std::wstring line;
    if (std::getline(file, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        return line;
    }
    return L"";
}

bool SaveRouteTargetToFile(const std::wstring& deviceName) {
    const std::wstring tmp = L"route_target.txt.tmp";
    {
        std::wofstream f(tmp);
        if (!f.is_open()) return false;
        f << deviceName << L"\n";
    }
    return MoveFileExW(tmp.c_str(), L"route_target.txt", MOVEFILE_REPLACE_EXISTING) != 0;
}

std::vector<DeviceConfig> LoadDevicesFromFile(const std::wstring& filename) {
    std::vector<DeviceConfig> result;
    std::wifstream file(filename);
    std::wstring line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        DeviceConfig cfg = ParseDeviceConfigLine(line);
        if (!cfg.name.empty()) result.push_back(std::move(cfg));
    }
    return result;
}

bool SaveDevicesToFile(const std::wstring& filename, const std::vector<DeviceConfig>& devices) {
    std::wstring tmp = filename + L".tmp";
    {
        std::wofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        for (const auto& d : devices) {
            out << SerializeDeviceConfig(d) << L"\n";
        }
        if (!out) return false;
    }
    if (!MoveFileExW(tmp.c_str(), filename.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

void SeedDefaultDevicesIfFirstRun() {
    auto devices = EnumerateRenderDevices();

    if (GetFileAttributesW(L"devices.txt") == INVALID_FILE_ATTRIBUTES) {
        std::vector<DeviceConfig> defaults;
        for (const auto& name : devices) {
            if (!NameMatchesDefaultMute(name)) continue;
            DeviceConfig cfg;
            cfg.name = name;
            cfg.filter = DeviceConfig::Filter::All;
            defaults.push_back(std::move(cfg));
        }
        SaveDevicesToFile(L"devices.txt", defaults);
    }

    if (GetFileAttributesW(L"route_target.txt") == INVALID_FILE_ATTRIBUTES) {
        for (const auto& name : devices) {
            std::wstring lower = name;
            for (auto& c : lower) c = towlower(c);
            if (lower.find(L"sonar") != std::wstring::npos &&
                lower.find(L"chat") != std::wstring::npos) {
                SaveRouteTargetToFile(name);
                break;
            }
        }
        if (GetFileAttributesW(L"route_target.txt") == INVALID_FILE_ATTRIBUTES)
            SaveRouteTargetToFile(L"");
    }
}

int checkToMute() {
    IMMDeviceEnumerator* pEnum = GetCachedDeviceEnumerator();
    if (!pEnum) return 1;

    std::wstring routeTarget = LoadRouteTargetFromFile();
    if (!routeTarget.empty()) {
        RouteDiscordAudioOutput(routeTarget, pEnum);
    }

    auto devices = LoadDevicesFromFile(L"devices.txt");
    if (devices.empty()) return 0;

    auto buildOtherPids = [](const std::wstring& exclude,
                              const std::unordered_map<std::wstring, std::vector<DWORD>>& byDevice) {
        std::vector<DWORD> other;
        for (const auto& kv : byDevice)
            if (kv.first != exclude)
                other.insert(other.end(), kv.second.begin(), kv.second.end());
        return other;
    };

    auto pidsByDevice = BuildDiscordPidsByDevice(pEnum);

    static const int MAX_RETRIES = 5;
    static const DWORD WAIT_MS = 1000;
    int retriesLeft = MAX_RETRIES;

    int check = 0;
    for (int i = 0; i < (int)devices.size(); i++) {
        if (g_exitEvent && WaitForSingleObject(g_exitEvent, 0) == WAIT_OBJECT_0) {
            #ifdef DEBUG
            OutputDebugStringA("checkToMute: shutdown requested, exiting early.\n");
            #endif
            return 0;
        }

        auto otherPids = buildOtherPids(devices[i].name, pidsByDevice);
        check = MuteDiscordOnDevice(devices[i], otherPids, pEnum);
        if (check == 2) {
            if (retriesLeft <= 0) {
                #ifdef DEBUG
                OutputDebugStringA("checkToMute: retry budget exhausted, giving up.\n");
                #endif
                break;
            }
            retriesLeft--;
            #ifdef DEBUG
            std::wstringstream dss;
            dss << L"checkToMute: device returned retry, " << retriesLeft << L" retries left.\n";
            OutputDebugStringW(dss.str().c_str());
            #endif
            if (g_exitEvent && WaitForSingleObject(g_exitEvent, WAIT_MS) == WAIT_OBJECT_0) {
                #ifdef DEBUG
                OutputDebugStringA("checkToMute: shutdown requested during wait, exiting.\n");
                #endif
                return 0;
            }
            i = -1;
            pidsByDevice = BuildDiscordPidsByDevice(pEnum);
        }
    }
    return 0;
}

void RunDiagnostic(HWND owner) {
    std::wstringstream ss;
    {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        localtime_s(&tm, &t);
        wchar_t stamp[64];
        wcsftime(stamp, 64, L"%Y-%m-%d %H:%M:%S", &tm);
        ss << L"Discord session diagnostic - " << stamp << L"\n\n";
    }

    auto configs = LoadDevicesFromFile(L"devices.txt");
    std::vector<std::wstring> deviceNames;
    if (configs.empty()) {
        deviceNames = EnumerateRenderDevices();
    } else {
        for (const auto& c : configs) deviceNames.push_back(c.name);
    }

    auto pidsByDevice = BuildDiscordPidsByDevice();

    for (const auto& devName : deviceNames) {
        ss << L"Device: " << devName << L"\n";
        auto sessions = EnumerateDiscordSessions(devName);
        if (sessions.empty()) {
            ss << L"  (no Discord sessions found)\n\n";
            continue;
        }
        std::vector<DWORD> otherPids;
        for (const auto& kv : pidsByDevice)
            if (kv.first != devName)
                otherPids.insert(otherPids.end(), kv.second.begin(), kv.second.end());
        for (const auto& s : sessions) {
            bool shared = std::find(otherPids.begin(), otherPids.end(), s.pid) != otherPids.end();
            ss << L"  Session #" << s.ordinalOnDevice << L"\n";
            ss << L"    PID:                       " << s.pid
               << (shared ? L"  (shared - stream candidate)" : L"  (exclusive - keep audible)") << L"\n";
            ss << L"    DisplayName:               " << (s.displayName.empty() ? L"(empty)" : s.displayName) << L"\n";
            ss << L"    IconPath:                  " << (s.iconPath.empty() ? L"(empty)" : s.iconPath) << L"\n";
            ss << L"    SessionIdentifier:         " << s.sessionIdentifier << L"\n";
            ss << L"    StableFingerprint:         " << StableSessionFingerprint(s.sessionIdentifier) << L"\n";
            ss << L"    SessionInstanceIdentifier: " << s.sessionInstanceIdentifier << L"\n";
            ss << L"    GroupingParam:             " << GuidToWString(s.groupingParam) << L"\n";
        }
        ss << L"\n";
    }

    std::wstring logPath = L"discord-sessions.log";
    {
        std::wofstream out(logPath, std::ios::trunc);
        if (!out) {
            MessageBoxW(owner, L"Failed to write discord-sessions.log", L"Diagnostic",
                MB_OK | MB_ICONERROR);
            return;
        }
        out << ss.str();
    }
    ShellExecuteW(owner, L"open", logPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

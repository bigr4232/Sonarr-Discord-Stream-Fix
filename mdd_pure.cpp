#include "mdd_pure.h"

// ---------- Data model ----------
// (defined in header)

// ---------- Pure function implementations ----------

std::wstring StableSessionFingerprint(const std::wstring& sid) {
    if (sid.empty()) return L"";
    size_t lastPct = sid.rfind(L"%b");
    std::wstring tail;
    if (lastPct != std::wstring::npos) {
        // Walk backwards to find the previous %b so we capture {guid}%b<flags>
        size_t prev = sid.rfind(L"%b", lastPct > 0 ? lastPct - 1 : 0);
        if (prev != std::wstring::npos && prev < lastPct) {
            tail = sid.substr(prev + 2);
        } else {
            tail = sid.substr(lastPct + 2);
        }
    }
    if (tail.empty()) {
        // Fallback: take substring after last '|' if present
        size_t bar = sid.rfind(L'|');
        tail = (bar == std::wstring::npos) ? sid : sid.substr(bar + 1);
    }
    return tail;
}

DeviceConfig ParseDeviceConfigLine(const std::wstring& rawLine) {
    std::wstring line = rawLine;
    if (!line.empty() && line.back() == L'\r') line.pop_back();

    DeviceConfig cfg;
    size_t bar = line.find(L'|');
    if (bar == std::wstring::npos) {
        cfg.name = line;
        // Bare lines from old installs: mute all (backwards compat)
        cfg.filter = DeviceConfig::Filter::All;
        return cfg;
    }
    cfg.name = line.substr(0, bar);
    std::wstring spec = line.substr(bar + 1);
    if (spec == L"stream") {
        cfg.filter = DeviceConfig::Filter::StreamOnly;
    } else if (spec.rfind(L"sid:", 0) == 0) {
        cfg.filter = DeviceConfig::Filter::ByFingerprint;
        cfg.fingerprint = spec.substr(4);
    } else if (spec.rfind(L"ord:", 0) == 0) {
        cfg.filter = DeviceConfig::Filter::ByOrdinal;
        try { cfg.ordinal = std::stoi(spec.substr(4)); } catch (...) { cfg.ordinal = -1; }
    }
    return cfg;
}

std::wstring SerializeDeviceConfig(const DeviceConfig& cfg) {
    std::wstring line = cfg.name;
    switch (cfg.filter) {
        case DeviceConfig::Filter::StreamOnly:
            line += L"|stream";
            break;
        case DeviceConfig::Filter::ByFingerprint:
            if (!cfg.fingerprint.empty()) line += L"|sid:" + cfg.fingerprint;
            break;
        case DeviceConfig::Filter::ByOrdinal:
            if (cfg.ordinal >= 0) line += L"|ord:" + std::to_wstring(cfg.ordinal);
            break;
        default: break;
    }
    return line;
}

bool NameMatchesDefaultMute(const std::wstring& deviceName) {
    std::wstring lower;
    lower.reserve(deviceName.size());
    for (wchar_t c : deviceName) {
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        lower.push_back(c);
    }
    if (lower.find(L"steelseries sonar - microphone") != std::wstring::npos) return true;
    if (lower.find(L"arctis nova") != std::wstring::npos) return true;
    return false;
}

std::wstring FilterSummary(const DeviceConfig& cfg) {
    switch (cfg.filter) {
        case DeviceConfig::Filter::StreamOnly:
            return L"Stream only (auto)";
        case DeviceConfig::Filter::All:
            return L"All Discord sessions";
        case DeviceConfig::Filter::ByFingerprint: {
            std::wstring s = L"Specific: ";
            s += (cfg.fingerprint.size() > 20 ? cfg.fingerprint.substr(0, 20) + L"..." : cfg.fingerprint);
            return s;
        }
        case DeviceConfig::Filter::ByOrdinal:
            return L"Ordinal #" + std::to_wstring(cfg.ordinal);
    }
    return L"";
}

// ---------- Utility helpers ----------

std::wstring ToLowerCopy(const std::wstring& value) {
    std::wstring lower = value;
    for (auto& c : lower) c = towlower(c);
    return lower;
}

std::wstring HResultToWString(HRESULT hr) {
    wchar_t buf[32] = {};
    swprintf_s(buf, L"0x%08X", static_cast<unsigned int>(hr));
    return buf;
}

std::wstring GuidToWString(const GUID& g) {
    wchar_t buf[64];
    swprintf_s(buf, L"{%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX}",
        g.Data1, g.Data2, g.Data3,
        g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
        g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

static const wchar_t* MMDEVAPI_TOKEN = L"\\\\?\\SWD#MMDEVAPI#";
static const wchar_t* POLICY_RENDER_INTERFACE_SUFFIX = L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
static const wchar_t* POLICY_CAPTURE_INTERFACE_SUFFIX = L"#{2eef81be-33fa-4800-9670-1cd474972c3f}";

std::wstring PackPolicyConfigDeviceId(const std::wstring& deviceId, EDataFlow flow) {
    if (deviceId.empty()) return L"";
    return std::wstring(MMDEVAPI_TOKEN) + deviceId +
        (flow == eCapture ? POLICY_CAPTURE_INTERFACE_SUFFIX : POLICY_RENDER_INTERFACE_SUFFIX);
}

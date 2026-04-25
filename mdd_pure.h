#pragma once
// mdd_pure.h - Pure (side-effect-free) functions extracted from main.cpp for testability.
//
// These functions contain no Windows GUI, COM, or file-system dependencies and can be
// unit-tested in isolation. main.cpp includes this header to share the implementations.

#include <windows.h>
#include <mmdeviceapi.h>
#include <string>
#include <vector>
#include <unordered_map>

// ---------- Data model ----------

struct DeviceConfig {
    std::wstring name;
    enum class Filter { All, ByFingerprint, ByOrdinal, StreamOnly } filter = Filter::All;
    std::wstring fingerprint;
    int ordinal = -1;
    bool offline = false;
};

// ---------- Pure function declarations ----------

/// Extract a stable fingerprint from a SessionIdentifier string.
/// Strips the rotating app-version path and keeps only the trailing GUID + flags.
std::wstring StableSessionFingerprint(const std::wstring& sid);

/// Parse a single line from devices.txt into a DeviceConfig.
DeviceConfig ParseDeviceConfigLine(const std::wstring& rawLine);

/// Serialize a DeviceConfig back to the single-line format used in devices.txt.
std::wstring SerializeDeviceConfig(const DeviceConfig& cfg);

/// Case-insensitive substring match against known default-mute device names.
bool NameMatchesDefaultMute(const std::wstring& deviceName);

/// Human-readable summary of a filter setting for display in the config dialog.
std::wstring FilterSummary(const DeviceConfig& cfg);

// ---------- Utility helpers ----------

/// Lowercase copy of a wide string.
std::wstring ToLowerCopy(const std::wstring& value);

/// Format an HRESULT as a hex wide string, e.g. L"0x80070005".
std::wstring HResultToWString(HRESULT hr);

/// Format a GUID as a wide string, e.g. L"{A1B2C3D4-...}".
std::wstring GuidToWString(const GUID& g);

/// Pack a raw MMDevice ID into the PolicyConfig-style form.
std::wstring PackPolicyConfigDeviceId(const std::wstring& deviceId, EDataFlow flow);

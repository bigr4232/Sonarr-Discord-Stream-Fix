// test_config_serialization.cpp
// Tests for ParseDeviceConfigLine() and SerializeDeviceConfig() round-trips.

#include <gtest/gtest.h>
#include "mdd_pure.h"

// ---------------------------------------------------------------------------
// ParseDeviceConfigLine - basic parsing
// ---------------------------------------------------------------------------

TEST(ParseDeviceConfigLine, BareDeviceName_DefaultsToAll) {
    DeviceConfig cfg = ParseDeviceConfigLine(L"Speakers (Realtek Audio)");
    EXPECT_EQ(cfg.name, L"Speakers (Realtek Audio)");
    EXPECT_EQ(cfg.filter, DeviceConfig::Filter::All);
    EXPECT_EQ(cfg.fingerprint, L"");
    EXPECT_EQ(cfg.ordinal, -1);
}

TEST(ParseDeviceConfigLine, BareDeviceNameWithCarriageReturn) {
    // Windows line endings include \r\n; getline strips \n but not \r.
    DeviceConfig cfg = ParseDeviceConfigLine(L"Headphones (SteelSeries)\r");
    EXPECT_EQ(cfg.name, L"Headphones (SteelSeries)");
    EXPECT_EQ(cfg.filter, DeviceConfig::Filter::All);
}

TEST(ParseDeviceConfigLine, StreamOnlySpecifier) {
    DeviceConfig cfg = ParseDeviceConfigLine(L"VB-Cable Output|stream");
    EXPECT_EQ(cfg.name, L"VB-Cable Output");
    EXPECT_EQ(cfg.filter, DeviceConfig::Filter::StreamOnly);
}

TEST(ParseDeviceConfigLine, FingerprintSpecifier) {
    DeviceConfig cfg = ParseDeviceConfigLine(L"Device A|sid:{AAAA-BBBB}%b1");
    EXPECT_EQ(cfg.name, L"Device A");
    EXPECT_EQ(cfg.filter, DeviceConfig::Filter::ByFingerprint);
    EXPECT_EQ(cfg.fingerprint, L"{AAAA-BBBB}%b1");
}

TEST(ParseDeviceConfigLine, OrdinalSpecifier) {
    DeviceConfig cfg = ParseDeviceConfigLine(L"Device B|ord:3");
    EXPECT_EQ(cfg.name, L"Device B");
    EXPECT_EQ(cfg.filter, DeviceConfig::Filter::ByOrdinal);
    EXPECT_EQ(cfg.ordinal, 3);
}

TEST(ParseDeviceConfigLine, OrdinalZero) {
    DeviceConfig cfg = ParseDeviceConfigLine(L"Device|ord:0");
    EXPECT_EQ(cfg.filter, DeviceConfig::Filter::ByOrdinal);
    EXPECT_EQ(cfg.ordinal, 0);
}

TEST(ParseDeviceConfigLine, InvalidOrdinal_DefaultsToMinusOne) {
    DeviceConfig cfg = ParseDeviceConfigLine(L"Device|ord:not_a_number");
    EXPECT_EQ(cfg.filter, DeviceConfig::Filter::ByOrdinal);
    EXPECT_EQ(cfg.ordinal, -1);
}

TEST(ParseDeviceConfigLine, NegativeOrdinal) {
    DeviceConfig cfg = ParseDeviceConfigLine(L"Device|ord:-5");
    EXPECT_EQ(cfg.filter, DeviceConfig::Filter::ByOrdinal);
    EXPECT_EQ(cfg.ordinal, -5);
}

TEST(ParseDeviceConfigLine, UnknownSpecifier_DefaultsToAll) {
    // If the spec after '|' doesn't match any known prefix, filter stays All.
    DeviceConfig cfg = ParseDeviceConfigLine(L"Device|unknown_value");
    EXPECT_EQ(cfg.name, L"Device");
    EXPECT_EQ(cfg.filter, DeviceConfig::Filter::All);
}

TEST(ParseDeviceConfigLine, EmptyName_WithStreamSpecifier) {
    DeviceConfig cfg = ParseDeviceConfigLine(L"|stream");
    EXPECT_EQ(cfg.name, L"");
    EXPECT_EQ(cfg.filter, DeviceConfig::Filter::StreamOnly);
}

TEST(ParseDeviceConfigLine, DeviceNameContainingPipe) {
    // If the device name itself contains '|', only the first '|' is the delimiter.
    // The spec "B Device|stream" does not match any known prefix, so filter stays All.
    DeviceConfig cfg = ParseDeviceConfigLine(L"A|B Device|stream");
    EXPECT_EQ(cfg.name, L"A");
    EXPECT_EQ(cfg.filter, DeviceConfig::Filter::All);
}

TEST(ParseDeviceConfigLine, EmptyLine) {
    DeviceConfig cfg = ParseDeviceConfigLine(L"");
    EXPECT_EQ(cfg.name, L"");
    EXPECT_EQ(cfg.filter, DeviceConfig::Filter::All);
}

// ---------------------------------------------------------------------------
// SerializeDeviceConfig - basic serialization
// ---------------------------------------------------------------------------

TEST(SerializeDeviceConfig, AllFilter_NoSpecifier) {
    DeviceConfig cfg;
    cfg.name = L"My Device";
    cfg.filter = DeviceConfig::Filter::All;
    EXPECT_EQ(SerializeDeviceConfig(cfg), L"My Device");
}

TEST(SerializeDeviceConfig, StreamOnly_AppendsStream) {
    DeviceConfig cfg;
    cfg.name = L"VB-Cable";
    cfg.filter = DeviceConfig::Filter::StreamOnly;
    EXPECT_EQ(SerializeDeviceConfig(cfg), L"VB-Cable|stream");
}

TEST(SerializeDeviceConfig, ByFingerprint_AppendsSid) {
    DeviceConfig cfg;
    cfg.name = L"Device";
    cfg.filter = DeviceConfig::Filter::ByFingerprint;
    cfg.fingerprint = L"{GUID}%b1";
    EXPECT_EQ(SerializeDeviceConfig(cfg), L"Device|sid:{GUID}%b1");
}

TEST(SerializeDeviceConfig, ByFingerprint_EmptyFingerprint_NoSpecifier) {
    DeviceConfig cfg;
    cfg.name = L"Device";
    cfg.filter = DeviceConfig::Filter::ByFingerprint;
    cfg.fingerprint = L"";
    // Empty fingerprint means no specifier is appended.
    EXPECT_EQ(SerializeDeviceConfig(cfg), L"Device");
}

TEST(SerializeDeviceConfig, ByOrdinal_AppendsOrd) {
    DeviceConfig cfg;
    cfg.name = L"Device";
    cfg.filter = DeviceConfig::Filter::ByOrdinal;
    cfg.ordinal = 5;
    EXPECT_EQ(SerializeDeviceConfig(cfg), L"Device|ord:5");
}

TEST(SerializeDeviceConfig, ByOrdinal_Negative_NoSpecifier) {
    DeviceConfig cfg;
    cfg.name = L"Device";
    cfg.filter = DeviceConfig::Filter::ByOrdinal;
    cfg.ordinal = -1;
    // Negative ordinal means no specifier is appended.
    EXPECT_EQ(SerializeDeviceConfig(cfg), L"Device");
}

TEST(SerializeDeviceConfig, ByOrdinal_Zero) {
    DeviceConfig cfg;
    cfg.name = L"Device";
    cfg.filter = DeviceConfig::Filter::ByOrdinal;
    cfg.ordinal = 0;
    EXPECT_EQ(SerializeDeviceConfig(cfg), L"Device|ord:0");
}

// ---------------------------------------------------------------------------
// Round-trip tests - serialize then parse should produce equivalent config
// ---------------------------------------------------------------------------

TEST(ConfigRoundTrip, AllFilter) {
    DeviceConfig original;
    original.name = L"Speakers (Realtek Audio)";
    original.filter = DeviceConfig::Filter::All;

    std::wstring serialized = SerializeDeviceConfig(original);
    DeviceConfig parsed = ParseDeviceConfigLine(serialized);

    EXPECT_EQ(parsed.name, original.name);
    EXPECT_EQ(parsed.filter, original.filter);
}

TEST(ConfigRoundTrip, StreamOnly) {
    DeviceConfig original;
    original.name = L"CABLE-Output";
    original.filter = DeviceConfig::Filter::StreamOnly;

    std::wstring serialized = SerializeDeviceConfig(original);
    DeviceConfig parsed = ParseDeviceConfigLine(serialized);

    EXPECT_EQ(parsed.name, original.name);
    EXPECT_EQ(parsed.filter, original.filter);
}

TEST(ConfigRoundTrip, ByFingerprint) {
    DeviceConfig original;
    original.name = L"Headphones";
    original.filter = DeviceConfig::Filter::ByFingerprint;
    original.fingerprint = L"{d34cb318-f2a1-4e9c-b7f5-8a9c1d2e3f4a}%b131074";

    std::wstring serialized = SerializeDeviceConfig(original);
    DeviceConfig parsed = ParseDeviceConfigLine(serialized);

    EXPECT_EQ(parsed.name, original.name);
    EXPECT_EQ(parsed.filter, original.filter);
    EXPECT_EQ(parsed.fingerprint, original.fingerprint);
}

TEST(ConfigRoundTrip, ByOrdinal) {
    DeviceConfig original;
    original.name = L"Voicemeeter Output";
    original.filter = DeviceConfig::Filter::ByOrdinal;
    original.ordinal = 7;

    std::wstring serialized = SerializeDeviceConfig(original);
    DeviceConfig parsed = ParseDeviceConfigLine(serialized);

    EXPECT_EQ(parsed.name, original.name);
    EXPECT_EQ(parsed.filter, original.filter);
    EXPECT_EQ(parsed.ordinal, original.ordinal);
}

TEST(ConfigRoundTrip, ByOrdinalZero) {
    DeviceConfig original;
    original.name = L"Device";
    original.filter = DeviceConfig::Filter::ByOrdinal;
    original.ordinal = 0;

    std::wstring serialized = SerializeDeviceConfig(original);
    DeviceConfig parsed = ParseDeviceConfigLine(serialized);

    EXPECT_EQ(parsed.ordinal, 0);
}

TEST(ConfigRoundTrip, CarriageReturnHandled) {
    // Simulate reading from a file with Windows line endings.
    DeviceConfig original;
    original.name = L"Test Device";
    original.filter = DeviceConfig::Filter::StreamOnly;

    std::wstring serialized = SerializeDeviceConfig(original) + L"\r";
    DeviceConfig parsed = ParseDeviceConfigLine(serialized);

    EXPECT_EQ(parsed.name, original.name);
    EXPECT_EQ(parsed.filter, original.filter);
}

// ---------------------------------------------------------------------------
// Backwards compatibility - old format (bare device name) still works
// ---------------------------------------------------------------------------

TEST(ConfigBackwardsCompat, OldBareNameParsedAsAll) {
    // Old config files only had device names, no filter specifier.
    DeviceConfig cfg = ParseDeviceConfigLine(L"SteelSeries Arctis Nova Headphones");
    EXPECT_EQ(cfg.filter, DeviceConfig::Filter::All);
}

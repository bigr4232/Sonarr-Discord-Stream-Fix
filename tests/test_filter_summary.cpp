// test_filter_summary.cpp
// Tests for FilterSummary() - human-readable display strings for filter settings.

#include <gtest/gtest.h>
#include "mdd_pure.h"

// ---------------------------------------------------------------------------
// Each filter variant produces the expected display string
// ---------------------------------------------------------------------------

TEST(FilterSummary, AllFilter) {
    DeviceConfig cfg;
    cfg.filter = DeviceConfig::Filter::All;
    EXPECT_EQ(FilterSummary(cfg), L"All Discord sessions");
}

TEST(FilterSummary, StreamOnlyFilter) {
    DeviceConfig cfg;
    cfg.filter = DeviceConfig::Filter::StreamOnly;
    EXPECT_EQ(FilterSummary(cfg), L"Stream only (auto)");
}

TEST(FilterSummary, ByOrdinalFilter) {
    DeviceConfig cfg;
    cfg.filter = DeviceConfig::Filter::ByOrdinal;
    cfg.ordinal = 3;
    EXPECT_EQ(FilterSummary(cfg), L"Ordinal #3");
}

TEST(FilterSummary, ByOrdinalZero) {
    DeviceConfig cfg;
    cfg.filter = DeviceConfig::Filter::ByOrdinal;
    cfg.ordinal = 0;
    EXPECT_EQ(FilterSummary(cfg), L"Ordinal #0");
}

TEST(FilterSummary, ByOrdinalNegative) {
    DeviceConfig cfg;
    cfg.filter = DeviceConfig::Filter::ByOrdinal;
    cfg.ordinal = -1;
    EXPECT_EQ(FilterSummary(cfg), L"Ordinal #-1");
}

TEST(FilterSummary, ByFingerprintShort) {
    DeviceConfig cfg;
    cfg.filter = DeviceConfig::Filter::ByFingerprint;
    cfg.fingerprint = L"{GUID}%b1";
    // 9 chars - under the 20-char truncation threshold.
    EXPECT_EQ(FilterSummary(cfg), L"Specific: {GUID}%b1");
}

TEST(FilterSummary, ByFingerprintExactlyTwenty) {
    DeviceConfig cfg;
    cfg.filter = DeviceConfig::Filter::ByFingerprint;
    cfg.fingerprint = L"12345678901234567890";  // exactly 20 chars
    EXPECT_EQ(FilterSummary(cfg), L"Specific: 12345678901234567890");
}

TEST(FilterSummary, ByFingerprintTruncated) {
    DeviceConfig cfg;
    cfg.filter = DeviceConfig::Filter::ByFingerprint;
    cfg.fingerprint = L"{d34cb318-f2a1-4e9c-b7f5-8a9c1d2e3f4a}%b131074";  // 48 chars
    std::wstring result = FilterSummary(cfg);
    EXPECT_TRUE(result.find(L"Specific: ") == 0);
    EXPECT_TRUE(result.find(L"...") != std::wstring::npos);
    // Should be "Specific: " (10) + first 20 chars + "..." (3) = 33 chars total.
    EXPECT_EQ(result.size(), 33u);
}

TEST(FilterSummary, ByFingerprintEmpty) {
    DeviceConfig cfg;
    cfg.filter = DeviceConfig::Filter::ByFingerprint;
    cfg.fingerprint = L"";
    EXPECT_EQ(FilterSummary(cfg), L"Specific: ");
}

// ---------------------------------------------------------------------------
// Device name does not affect filter summary
// ---------------------------------------------------------------------------

TEST(FilterSummary, DeviceNameIgnored) {
    DeviceConfig cfgA;
    cfgA.name = L"Device A";
    cfgA.filter = DeviceConfig::Filter::All;

    DeviceConfig cfgB;
    cfgB.name = L"Completely Different Device Name";
    cfgB.filter = DeviceConfig::Filter::All;

    EXPECT_EQ(FilterSummary(cfgA), FilterSummary(cfgB));
}

// ---------------------------------------------------------------------------
// Default-initialized config
// ---------------------------------------------------------------------------

TEST(FilterSummary, DefaultConstructedConfig) {
    DeviceConfig cfg;  // defaults to Filter::All
    EXPECT_EQ(FilterSummary(cfg), L"All Discord sessions");
}
